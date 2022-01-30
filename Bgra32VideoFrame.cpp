#include "platform.h"
#include "Bgra32VideoFrame.h"
#include <cstdlib>

#define L 8294400
/* Bgra32VideoFrame class */

// Constructor generates empty pixel buffer
Bgra32VideoFrame::Bgra32VideoFrame(long width, long height, BMDFrameFlags flags) : 
	m_width(width), m_height(height), m_flags(flags), m_refCount(1)
{
	// Allocate pixel buffer
	m_pixelBuffer.resize(m_width*m_height*4);
}

HRESULT Bgra32VideoFrame::GetBytes(void **buffer)
{
	*buffer = (void*)m_pixelBuffer.data();
	return S_OK;
}

HRESULT	STDMETHODCALLTYPE Bgra32VideoFrame::QueryInterface(REFIID iid, LPVOID *ppv)
{
	HRESULT 		result = E_NOINTERFACE;

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
	
	else if (iid == IID_IDeckLinkVideoFrame)
	{
		*ppv = (IDeckLinkVideoFrame*)this;
		AddRef();
		result = S_OK;
	}

	return result;
}

ULONG STDMETHODCALLTYPE Bgra32VideoFrame::AddRef(void)
{
	return m_refCount.fetch_add(1);
}

ULONG STDMETHODCALLTYPE Bgra32VideoFrame::Release(void)
{
	ULONG		newRefValue;

	newRefValue = m_refCount.fetch_sub(1);
	if (newRefValue == 0)
	{
		delete this;
		return 0;
	}

	return newRefValue;
}
