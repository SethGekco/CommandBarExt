#pragma once

#include <windows.h>

class CommandBarExtDLL
{
public:
	static HANDLE hInstance;

	static const size_t readLength = 2048;
	static char readBuffer[readLength];
	static wchar_t wideBuffer[readLength];
};
