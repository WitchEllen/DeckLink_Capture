#include <chrono>
#include "platform.h"
#include "DeckLinkInputDevice.h"

static const std::chrono::seconds kValidFrameTimeout{5};

DeckLinkInputDevice::DeckLinkInputDevice(IDeckLink* device)
	: m_deckLink(device), m_deckLinkInput(NULL), m_cancelCapture(false), m_refCount(1)
{
	m_deckLink->AddRef();
}

DeckLinkInputDevice::~DeckLinkInputDevice()
{
	if (m_deckLinkInput != NULL)
	{
		m_deckLinkInput->Release();
		m_deckLinkInput = NULL;
	}

	while(!m_modeList.empty())
	{
		m_modeList.back()->Release();
		m_modeList.pop_back();
	}

	if (m_deckLink != NULL)
	{
		m_deckLink->Release();
		m_deckLink = NULL;
	}
}

HRESULT DeckLinkInputDevice::Init()
{
	HRESULT							result;
	IDeckLinkDisplayModeIterator*	displayModeIterator	= NULL;
	IDeckLinkDisplayMode*			displayMode			= NULL;
	dlstring_t						deviceNameStr;

	result = m_deckLink->QueryInterface(IID_IDeckLinkInput, (void**)&m_deckLinkInput);
	if (result != S_OK)
	{
		fprintf(stderr, "Unable to get IDeckLinkInput interface\n");
		goto bail;
	}

	// Retrieve and cache mode list
	result = m_deckLinkInput->GetDisplayModeIterator(&displayModeIterator);
	if (result != S_OK)
	{
		fprintf(stderr, "Unable to get IDeckLinkDisplayModeIterator interface\n");
		goto bail;
	}

	while (displayModeIterator->Next(&displayMode) == S_OK)
	{
		m_modeList.push_back(displayMode);
	}

	// Get device name
	result = m_deckLink->GetDisplayName(&deviceNameStr);
	if (result == S_OK)
	{
		m_deviceName = DlToStdString(deviceNameStr);
		DeleteString(deviceNameStr);
	}
	else
	{
		m_deviceName = "DeckLink";
	}

bail:
	if (displayModeIterator != NULL)
	{
		displayModeIterator->Release();
		displayModeIterator = NULL;
	}

	return result;
}

HRESULT DeckLinkInputDevice::StartCapture(BMDDisplayMode displayMode, BMDPixelFormat pixelFormat, bool enableFormatDetection)
{
	HRESULT result;
	BMDVideoInputFlags inputFlags = bmdVideoInputFlagDefault;

	m_prevInputFrameValid = false;
	
	if (enableFormatDetection)
		inputFlags |= bmdVideoInputEnableFormatDetection;

	// Set capture callback
	m_deckLinkInput->SetCallback(this);

	// Set the video input mode
	result = m_deckLinkInput->EnableVideoInput(displayMode, pixelFormat, inputFlags);
	if (result != S_OK)
	{
		fprintf(stderr, "Unable to enable video input. Perhaps, the selected device is currently in-use.\n");
		goto bail;
	}

	// Start the capture
	result = m_deckLinkInput->StartStreams();
	if (result != S_OK)
	{
		fprintf(stderr, "Unable to start input streams\n");
		goto bail;
	}

bail:
	return result;
}

void DeckLinkInputDevice::CancelCapture()
{
	{
		// signal cancel flag to terminate wait condition
		std::lock_guard<std::mutex> lock(m_deckLinkInputMutex);
		m_cancelCapture = true;
	}
	m_deckLinkInputCondition.notify_one();
}

void DeckLinkInputDevice::StopCapture()
{
	if (m_deckLinkInput != NULL)
	{
		// Unregister capture callback
		m_deckLinkInput->SetCallback(NULL);
		
		{
			// Clear video frame queue
			std::lock_guard<std::mutex> lock(m_deckLinkInputMutex);
			while (!m_videoFrameQueue.empty())
			{
				m_videoFrameQueue.front()->Release();
				m_videoFrameQueue.pop();
			}
		}

		// Stop the capture
		m_deckLinkInput->StopStreams();

		// Disable video input
		m_deckLinkInput->DisableVideoInput();
	}
}

bool DeckLinkInputDevice::WaitForVideoFrameArrived(IDeckLinkVideoFrame** frame, bool& captureCancelled)
{
	std::unique_lock<std::mutex> lock(m_deckLinkInputMutex);
	if (!m_deckLinkInputCondition.wait_for(lock, kValidFrameTimeout, [&]{ return !m_videoFrameQueue.empty() || m_cancelCapture; }))
		// wait_for timeout
		return false;

	if (!m_videoFrameQueue.empty())
	{
		*frame = m_videoFrameQueue.front();
		m_videoFrameQueue.pop();
	}

	captureCancelled = m_cancelCapture;
	return true;
}

HRESULT DeckLinkInputDevice::VideoInputFormatChanged(/* in */ BMDVideoInputFormatChangedEvents notificationEvents, /* in */ IDeckLinkDisplayMode *newMode, /* in */ BMDDetectedVideoInputFormatFlags detectedSignalFlags)
{	
	HRESULT			result;
	BMDPixelFormat	pixelFormat = bmdFormat10BitYUV;
	dlstring_t		displayModeNameStr;

	if (detectedSignalFlags & bmdDetectedVideoInputRGB444)
		pixelFormat = bmdFormat10BitRGB;

	// Stop the capture
	m_deckLinkInput->StopStreams();

	// Set the detected video input mode
	result = m_deckLinkInput->EnableVideoInput(newMode->GetDisplayMode(), pixelFormat, bmdVideoInputEnableFormatDetection);
	if (result != S_OK)
	{
		fprintf(stderr, "Unable to re-enable video input on auto-format detection"); 
		goto bail;
	}

	// Restart the capture
	result = m_deckLinkInput->StartStreams();
	if (result != S_OK)
	{
		fprintf(stderr, "Unable to restart streams on auto-format detection"); 
		goto bail;
	}		

	result = newMode->GetName(&displayModeNameStr);

	if (result == S_OK)
	{
		fprintf(stderr, "Video format changed to %s %s\n", DlToCString(displayModeNameStr), (detectedSignalFlags & bmdDetectedVideoInputRGB444) ? "RGB" : "YUV");
		DeleteString(displayModeNameStr);
	}
	else
		fprintf(stderr, "Unable to get new video format name\n");

bail:
	return result;
}

HRESULT DeckLinkInputDevice::VideoInputFrameArrived(/* in */ IDeckLinkVideoInputFrame* videoFrame, /* in */ IDeckLinkAudioInputPacket* audioPacket)
{
	if (videoFrame)
	{
		bool inputFrameValid = ((videoFrame->GetFlags() & bmdFrameHasNoInputSource) == 0);

		// Detect change in input signal, restart stream when valid stream detected 
		if (inputFrameValid && !m_prevInputFrameValid)
		{
			m_deckLinkInput->StopStreams();
			m_deckLinkInput->FlushStreams();
			m_deckLinkInput->StartStreams();
		}

		if (inputFrameValid && m_prevInputFrameValid)
		{
			// If valid frame, add to queue for processing and notify
			videoFrame->AddRef();
			{
				std::lock_guard<std::mutex> lock(m_deckLinkInputMutex);
				m_videoFrameQueue.push(videoFrame);
			}
			m_deckLinkInputCondition.notify_one();
		}

		m_prevInputFrameValid = inputFrameValid;
	}

	return S_OK;
}

HRESULT	STDMETHODCALLTYPE DeckLinkInputDevice::QueryInterface(REFIID iid, LPVOID *ppv)
{
	HRESULT			result = E_NOINTERFACE;

	if (ppv == NULL)
		return E_INVALIDARG;

	// Initialise the return result
	*ppv = NULL;

	// Obtain the IUnknown interface and compare it the provided REFIID
	if (iid == IID_IUnknown)
	{
		*ppv = this;
		AddRef();
		result = S_OK;
	}
	else if (iid == IID_IDeckLinkInputCallback)
	{
		*ppv = (IDeckLinkInputCallback*)this;
		AddRef();
		result = S_OK;
	}

	return result;
}

ULONG STDMETHODCALLTYPE DeckLinkInputDevice::AddRef(void)
{
	return m_refCount.fetch_add(1);
}

ULONG STDMETHODCALLTYPE DeckLinkInputDevice::Release(void)
{
	int		newRefValue;

	newRefValue = m_refCount.fetch_sub(1);
	if (newRefValue == 0)
	{
		delete this;
		return 0;
	}

	return newRefValue;
}
