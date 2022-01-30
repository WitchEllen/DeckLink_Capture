#include <stdio.h>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <queue>
#include <thread>
#include <tuple>
#include <vector>
#include <string>
#include <atlstr.h>
#include <opencv2\opencv.hpp>

#include "platform.h"
#include "Bgra32VideoFrame.h"
#include "DeckLinkInputDevice.h"
#include "DeckLinkAPI.h"

#define N 4

// Pixel format tuple encoding {BMDPixelFormat enum, Pixel format display name}
const std::vector<std::tuple<BMDPixelFormat, std::string>> kSupportedPixelFormats{
	std::make_tuple(bmdFormat8BitYUV, "8 bit YUV (4:2:2)"),
	std::make_tuple(bmdFormat10BitYUV, "10 bit YUV (4:2:2)"),
	std::make_tuple(bmdFormat8BitARGB, "8 bit ARGB (4:4:4)"),
	std::make_tuple(bmdFormat8BitBGRA, "8 bit BGRA (4:4:4)"),
	std::make_tuple(bmdFormat10BitRGB, "10 bit RGB (4:4:4)"),
	std::make_tuple(bmdFormat12BitRGB, "12 bit RGB (4:4:4)"),
	std::make_tuple(bmdFormat12BitRGBLE, "12 bit RGB (4:4:4) Little-Endian"),
	std::make_tuple(bmdFormat10BitRGBX, "10 bit RGBX (4:4:4)"),
	std::make_tuple(bmdFormat10BitRGBXLE, "10 bit RGBX (4:4:4) Little-Endian"),
};
enum
{
	kPixelFormatValue = 0,
	kPixelFormatString
};

void GetNextFilename(const std::string &path, const std::string &prefix, const std::string &suffix, std::string &nextFileName, const int &index)
{
	CString filename;
	filename.Format(_T("%s\\%s%.4d.%s"), CString(path.c_str()), CString(prefix.c_str()), index, CString(suffix.c_str()));
	nextFileName = std::string(CT2CA(filename.GetString()));
}

void CaptureStills(int ID,DeckLinkInputDevice *deckLinkInput, const int captureInterval, const int framesToCapture, const std::string captureDirectory, const std::string filenamePrefix, const std::string filenameSuffix)
{
	int captureFrameCount = -1;
	bool captureRunning = true;
	std::string outputFileName;

	IDeckLinkVideoFrame *receivedVideoFrame = NULL;
	IDeckLinkVideoConversion *deckLinkFrameConverter = NULL;
	Bgra32VideoFrame *bgra32Frame = NULL;
	void *bytes = NULL;

	// Create frame conversion instance
	if (GetDeckLinkVideoConversion(&deckLinkFrameConverter) != S_OK)
		return;

	while (captureRunning)
	{
		bool captureCancelled;

		if (deckLinkInput == NULL)
			break;

		captureFrameCount++;

		if (!deckLinkInput->WaitForVideoFrameArrived(&receivedVideoFrame, captureCancelled))
		{
			fprintf(stderr, "Device #%d Timeout waiting for valid frame #%d\n", ID, captureFrameCount);
			captureFrameCount--;
			// captureRunning = false;
		}
		else if (captureCancelled)
			captureRunning = false;
		else if (captureFrameCount % captureInterval == 0)
		{
			GetNextFilename(captureDirectory, filenamePrefix, filenameSuffix, outputFileName, captureFrameCount / captureInterval);
			// fprintf(stderr, "Device #%d Capturing frame #%d\n", i, captureFrameCounts[i]);

			if (receivedVideoFrame->GetPixelFormat() == bmdFormat8BitBGRA)
			{
				// Frame is already 8-bit BGRA - no conversion required
				// bgra32Frame = receivedVideoFrame;
				// bgra32Frame->AddRef();
				bgra32Frame = new Bgra32VideoFrame(receivedVideoFrame->GetWidth(), receivedVideoFrame->GetHeight(), receivedVideoFrame->GetFlags());
			}
			else
			{
				bgra32Frame = new Bgra32VideoFrame(receivedVideoFrame->GetWidth(), receivedVideoFrame->GetHeight(), receivedVideoFrame->GetFlags());

				if (FAILED(deckLinkFrameConverter->ConvertFrame(receivedVideoFrame, bgra32Frame)))
				{
					fprintf(stderr, "Device #%d frame #%d conversion to BGRA was unsuccessful\n", ID, captureFrameCount);
					// captureRunning = false;
				}
			}
			bgra32Frame->GetBytes(&bytes);
			cv::Mat mat(receivedVideoFrame->GetHeight(), receivedVideoFrame->GetWidth(), CV_8UC4, bytes);
			// cv::cvtColor(mat, mat, cv::COLOR_BGRA2RGB);
			// cv::imwrite(outputFileName, mat);

			if (!cv::imwrite(outputFileName, mat))
			{
				fprintf(stderr, "Device #%d frame #%d encoding to file unsuccessfully\n", ID, captureFrameCount);
				// captureRunning = false;
			}
			delete bgra32Frame;
			// bgra32Frame->Release();

			if (framesToCapture != -1 && (captureFrameCount / captureInterval) >= framesToCapture)
			{
				fprintf(stderr, "Device #%d Completed Capture\n", ID);
				captureRunning = false;
			}
		}

		if (receivedVideoFrame != NULL)
		{
			receivedVideoFrame->Release();
			receivedVideoFrame = NULL;
		}
	}

	if (deckLinkFrameConverter != NULL)
	{
		deckLinkFrameConverter->Release();
		deckLinkFrameConverter = NULL;
	}

	// stop add new frame to queue
	deckLinkInput->StopCapture();
}

int bail(DeckLinkInputDevice *selectedDeckLinkInputs[], IDeckLinkIterator *deckLinkIterator, int exitStatus)
{
	for (int i = 0; i < N; i++)
	{
		DeckLinkInputDevice *sdli = selectedDeckLinkInputs[i];
		if (sdli != NULL)
		{
			sdli->Release();
			sdli = NULL;
		}
	}

	if (deckLinkIterator != NULL)
	{
		deckLinkIterator->Release();
		deckLinkIterator = NULL;
	}

	CoUninitialize();
	return exitStatus;
}

int main(int argc, char *argv[])
{
	// Configuration Flags
	int deckLinkIndexs[N] = {0, 0, 0, 0};
	int displayModeIndexs[N] = {-1, -1, -1, -1};
	int framesToCaptures[N] = {1, 1, 1, 1};
	int captureIntervals[N] = {1, 1, 1, 1};
	int pixelFormatIndexs[N] = {0, 0, 0, 0};
	bool enableFormatDetections[N] = {false, false, false, false};
	std::string filenamePrefixs[N] = {"d0_", "d1_", "d2_", "d3_"};
	std::string filenameSuffixs[N] = {"jpeg", "jpeg", "jpeg", "jpeg"};
	std::string captureDirectorys[N] = {"./output/d0", "./output/d1", "./output/d2", "./output/d3"};

	HRESULT result;
	int exitStatus = 1;
	bool supportsFormatDetection = false;

	std::thread captureStillsThreads[N];
	std::thread keyPressThread;

	IDeckLinkIterator *deckLinkIterator = NULL;
	IDeckLink *deckLink = NULL;
	DeckLinkInputDevice *selectedDeckLinkInputs[N] = {NULL, NULL, NULL, NULL};

	BMDDisplayMode selectedDisplayMode = bmdModeNTSC;
	std::string selectedDisplayModeName;
	std::vector<std::string> deckLinkDeviceNames;

	// load config
	std::fstream fin("config.txt", std::ios::in);
	if (!fin)
		return exitStatus;
	for (int i = 0; i < N; i++)
	{
		fin >> deckLinkIndexs[i] >> displayModeIndexs[i] >> framesToCaptures[i] >> captureIntervals[i] >> pixelFormatIndexs[0];
		fin >> filenamePrefixs[i];
		fin >> filenameSuffixs[i];
		fin >> captureDirectorys[i];
	}
	// end

	// Initialize COM on this thread
	result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	if (FAILED(result))
	{
		fprintf(stderr, "Initialization of COM failed - result = %08x.\n", result);
		return bail(selectedDeckLinkInputs, deckLinkIterator, exitStatus);
	}

	result = GetDeckLinkIterator(&deckLinkIterator);
	if (result != S_OK)
		return bail(selectedDeckLinkInputs, deckLinkIterator, exitStatus);
	// end

	// Obtain the required DeckLink device
	for (int i = 0; i < N && (result = deckLinkIterator->Next(&deckLink)) == S_OK; i++)
	{
		dlstring_t deckLinkName;

		result = deckLink->GetDisplayName(&deckLinkName);
		if (result == S_OK)
		{
			deckLinkDeviceNames.push_back(DlToStdString(deckLinkName));
			DeleteString(deckLinkName);
		}

		if (deckLinkIndexs[i] == 1)
		{
			// Check that selected device supports capture
			IDeckLinkProfileAttributes *deckLinkAttributes = NULL;
			int64_t ioSupportAttribute = 0;
			dlbool_t formatDetectionSupportAttribute;

			result = deckLink->QueryInterface(IID_IDeckLinkProfileAttributes, (void **)&deckLinkAttributes);

			if (result != S_OK)
			{
				fprintf(stderr, "Unable to get IDeckLinkAttributes interface\n");
				return bail(selectedDeckLinkInputs, deckLinkIterator, exitStatus);
			}

			// Check whether device supports cpature
			result = deckLinkAttributes->GetInt(BMDDeckLinkVideoIOSupport, &ioSupportAttribute);

			if ((result != S_OK) || ((ioSupportAttribute & bmdDeviceSupportsCapture) == 0))
			{
				fprintf(stderr, "Selected device does not support capture\n");
				return bail(selectedDeckLinkInputs, deckLinkIterator, exitStatus);
			}
			else
			{
				// Check if input mode detection is supported.
				result = deckLinkAttributes->GetFlag(BMDDeckLinkSupportsInputFormatDetection, &formatDetectionSupportAttribute);
				supportsFormatDetection = (result == S_OK) && (formatDetectionSupportAttribute != FALSE);

				selectedDeckLinkInputs[i] = new DeckLinkInputDevice(deckLink);
				//long long tmp = 0;
				//deckLinkAttributes->GetInt(BMDDeckLinkSubDeviceIndex, &tmp);
				//printf("%d\n", tmp);
			}
			deckLinkAttributes->Release();
		}
		deckLink->Release();
	}

	// start configuration
	for (int i = 0; i < N; i++)
	{
		if (deckLinkIndexs[i] != 1)
			continue;

		// Get display modes from the selected decklink output
		if (selectedDeckLinkInputs[i] != NULL)
		{
			result = selectedDeckLinkInputs[i]->Init();
			if (result != S_OK)
			{
				fprintf(stderr, "Unable to initialize DeckLink input interface");
				return bail(selectedDeckLinkInputs, deckLinkIterator, exitStatus);
			}

			// Get the display mode
			if ((displayModeIndexs[i] < -1) || (displayModeIndexs[i] >= (int)selectedDeckLinkInputs[i]->GetDisplayModeList().size()))
			{
				fprintf(stderr, "You must select a valid display mode\n");
				return bail(selectedDeckLinkInputs, deckLinkIterator, exitStatus);
			}
			else if (displayModeIndexs[i] == -1)
			{
				if (!supportsFormatDetection)
				{
					fprintf(stderr, "Format detection is not supported on this device\n");
					return bail(selectedDeckLinkInputs, deckLinkIterator, exitStatus);
				}
				else
				{
					enableFormatDetections[i] = true;

					// Format detection still needs a valid mode to start with
					selectedDisplayMode = bmdModeNTSC;
					selectedDisplayModeName = "Automatic mode detection";
					pixelFormatIndexs[i] = 0;
				}
			}
			else if ((pixelFormatIndexs[i] < 0) || (pixelFormatIndexs[i] >= (int)kSupportedPixelFormats.size()))
			{
				fprintf(stderr, "You must select a valid pixel format\n");
				return bail(selectedDeckLinkInputs, deckLinkIterator, exitStatus);
			}
			else
			{
				dlbool_t displayModeSupported;
				dlstring_t displayModeNameStr;
				IDeckLinkDisplayMode *displayMode = selectedDeckLinkInputs[i]->GetDisplayModeList()[displayModeIndexs[i]];

				result = displayMode->GetName(&displayModeNameStr);
				if (result == S_OK)
				{
					selectedDisplayModeName = DlToStdString(displayModeNameStr);
					DeleteString(displayModeNameStr);
				}

				selectedDisplayMode = displayMode->GetDisplayMode();

				// Check display mode is supported with given options
				result = selectedDeckLinkInputs[i]->GetDeckLinkInput()->DoesSupportVideoMode(bmdVideoConnectionUnspecified,
																							 selectedDisplayMode,
																							 std::get<kPixelFormatValue>(kSupportedPixelFormats[pixelFormatIndexs[i]]),
																							 bmdSupportedVideoModeDefault,
																							 &displayModeSupported);
				if ((result != S_OK) || (!displayModeSupported))
				{
					fprintf(stderr, "Display mode %s with pixel format %s is not supported by device\n",
							selectedDisplayModeName.c_str(),
							std::get<kPixelFormatString>(kSupportedPixelFormats[pixelFormatIndexs[i]]).c_str());
					return bail(selectedDeckLinkInputs, deckLinkIterator, exitStatus);
				}
			}
		}
		else
		{
			fprintf(stderr, "Invalid input device selected\n");
			return bail(selectedDeckLinkInputs, deckLinkIterator, exitStatus);
		}
	}

	for (int i = 0; i < N; i++)
	{
		if (deckLinkIndexs[i] != 1)
			continue;

		// Start capturing
		result = selectedDeckLinkInputs[i]->StartCapture(selectedDisplayMode, std::get<kPixelFormatValue>(kSupportedPixelFormats[pixelFormatIndexs[i]]), enableFormatDetections[i]);
		if (result != S_OK)
			return bail(selectedDeckLinkInputs, deckLinkIterator, exitStatus);

		// Print the selected configuration
		fprintf(stderr, "Capturing with the following configuration:\n"
						" - Capture device: %s\n"
						" - Video mode: %s\n"
						" - Pixel format: %s\n"
						" - Frames to capture: %d\n"
						" - Capture interval: %d\n"
						" - Filename prefix: %s\n"
						" - Capture directory: %s\n",
				selectedDeckLinkInputs[i]->GetDeviceName().c_str(),
				selectedDisplayModeName.c_str(),
				std::get<kPixelFormatString>(kSupportedPixelFormats[pixelFormatIndexs[i]]).c_str(),
				framesToCaptures[i],
				captureIntervals[i],
				filenamePrefixs[i].c_str(),
				captureDirectorys[i].c_str());

		// Start thread for capture processing
		captureStillsThreads[i] = std::thread([&] {
			CaptureStills(i,selectedDeckLinkInputs[i], captureIntervals[i], framesToCaptures[i], captureDirectorys[i], filenamePrefixs[i], filenameSuffixs[i]);
		});
	}

	fprintf(stderr, "Starting capture, press <RETURN> to stop/exit\n");

	keyPressThread = std::thread([&] {
		getchar();
		for (int i = 0; i < N; i++)
			if (deckLinkIndexs[i] == 1)
				selectedDeckLinkInputs[i]->CancelCapture();
	});

	// Wait on return of main capture stills thread
	for (int i = 0; i < N; i++)
	{
		if (deckLinkIndexs[i] == 1)
		{
			captureStillsThreads[i].join();
			// selectedDeckLinkInputs[i]->StopCapture();
		}
	}

	keyPressThread.join();

	// All Okay.
	exitStatus = 0;

	return bail(selectedDeckLinkInputs, deckLinkIterator, exitStatus);
}
