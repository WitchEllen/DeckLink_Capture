#include "platform.h"

HRESULT GetDeckLinkIterator(IDeckLinkIterator **deckLinkIterator)
{
	HRESULT result = S_OK;

	// Create an IDeckLinkIterator object to enumerate all DeckLink cards in the system
	result = CoCreateInstance(CLSID_CDeckLinkIterator, NULL, CLSCTX_ALL, IID_IDeckLinkIterator, (void**)deckLinkIterator);
	if (FAILED(result))
	{
		fprintf(stderr, "A DeckLink iterator could not be created.  The DeckLink drivers may not be installed.\n");
	}

	return result;
}

HRESULT GetDeckLinkVideoConversion(IDeckLinkVideoConversion **deckLinkVideoConversion)
{
	HRESULT result = S_OK;

	result = CoCreateInstance(CLSID_CDeckLinkVideoConversion, NULL, CLSCTX_ALL, IID_IDeckLinkVideoConversion, (void**)deckLinkVideoConversion);
	if (FAILED(result))
	{
		fprintf(stderr, "A DeckLink video conversion interface could not be created.\n");
	}

	return result;
}