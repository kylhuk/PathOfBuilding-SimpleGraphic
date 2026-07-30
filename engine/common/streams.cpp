// SimpleGraphic Engine
// (c) David Gowor, 2014
//
// Module: Streams
//

#include "common.h"

#include <algorithm>
#include <cstdint>
#include <limits>

#ifndef _WIN32
#include <sys/types.h>
#endif

namespace {

bool SeekFile(FILE* file, size_t position, int mode)
{
#ifdef _WIN32
	if (position > static_cast<size_t>((std::numeric_limits<__int64>::max)())) {
		return false;
	}
	return _fseeki64(file, static_cast<__int64>(position), mode) == 0;
#else
	if (position > static_cast<size_t>((std::numeric_limits<off_t>::max)())) {
		return false;
	}
	return fseeko(file, static_cast<off_t>(position), mode) == 0;
#endif
}

bool TellFile(FILE* file, size_t& position)
{
#ifdef _WIN32
	const __int64 value = _ftelli64(file);
#else
	const off_t value = ftello(file);
#endif
	if (value < 0 || static_cast<unsigned long long>(value) > (std::numeric_limits<size_t>::max)()) {
		return false;
	}
	position = static_cast<size_t>(value);
	return true;
}

} // namespace

// ============
// Memory Input
// ============

memInputStream_c::memInputStream_c()
{
	mem = NULL;
	memLen = memPos = 0;
	ownsMem = false;
}

memInputStream_c::memInputStream_c(ioStream_c* in)
	: memInputStream_c()
{
	MemInput(in);
}

memInputStream_c::memInputStream_c(byte* useMem, size_t useMemSize)
	: memInputStream_c()
{
	MemUse(useMem, useMemSize);
}

memInputStream_c::~memInputStream_c()
{
	MemFree();
}

size_t memInputStream_c::GetLen()
{
	return memLen;
}

size_t memInputStream_c::GetPos()
{
	return memPos;
}

bool memInputStream_c::Seek(size_t pos, int mode)
{
	size_t newPos;
	switch (mode) {
	case SEEK_SET:
		newPos = pos;
		break;
	case SEEK_CUR:
		if (pos > std::numeric_limits<size_t>::max() - memPos) {
			return true;
		}
		newPos = memPos + pos;
		break;
	case SEEK_END:
		if (pos > memLen) {
			return true;
		}
		newPos = memLen - pos;
		break;
	default:
		return true;
	}
	if (newPos <= memLen) {
		memPos = newPos;
		return false;
	}
	return true;
}

bool memInputStream_c::Read(void* out, size_t len)
{
	if (!out || memPos > memLen || len > memLen - memPos) {
		return true;
	}
	memcpy(out, mem + memPos, len);
	memPos+= len;
	return false;
}

bool memInputStream_c::MemInput(ioStream_c* in)
{
	MemFree();
	if (!in) {
		return true;
	}
	memLen = in->GetLen();
	if (memLen == 0) {
		return true;
	}
	mem = new byte[memLen];
	ownsMem = true;
	if (in->Seek(0, SEEK_SET) || in->Read(mem, memLen)) {
		MemFree();
		return true;
	}
	return false;
}

void memInputStream_c::MemCopy(byte* in, size_t len)
{
	MemFree();
	if (len && in) {
		mem = new byte[len];
		memLen = len;
		ownsMem = true;
		memcpy(mem, in, len);
	}
}
void memInputStream_c::MemUse(byte* in, size_t len)
{
	MemFree();
	if (!in && len != 0) {
		return;
	}
	mem = in;
	memLen = len;
	memPos = 0;
	ownsMem = false;
}

void memInputStream_c::MemFree()
{
	if (ownsMem) {
		delete[] mem;
	}
	mem = NULL;
	memLen = memPos = 0;
	ownsMem = false;
}

// =============
// Memory Output
// =============

memOutputStream_c::memOutputStream_c(size_t initSize)
{
	memLen = 0;
	memSize = (std::max)(initSize, static_cast<size_t>(16));
	memPos = 0;
	mem = new byte[memSize];
}

memOutputStream_c::~memOutputStream_c()
{
	delete[] mem;
}

size_t memOutputStream_c::GetLen()
{
	return memLen;
}

size_t memOutputStream_c::GetPos()
{
	return memPos;
}

bool memOutputStream_c::Seek(size_t pos, int mode)
{
	size_t newPos;
	switch (mode) {
	case SEEK_SET:
		newPos = pos;
		break;
	case SEEK_CUR:
		if (pos > std::numeric_limits<size_t>::max() - memPos) {
			return true;
		}
		newPos = memPos + pos;
		break;
	case SEEK_END:
		if (pos > memLen) {
			return true;
		}
		newPos = memLen - pos;
		break;
	default:
		return true;
	}
	memPos = newPos;
	return false;
}

bool memOutputStream_c::Write(const void* in, size_t len)
{
	if ((!in && len != 0) || memPos > std::numeric_limits<size_t>::max() - len) {
		return true;
	}
	const size_t required = memPos + len;
	if (required > memSize) {
		size_t newSize = memSize;
		while (newSize < required) {
			if (newSize > std::numeric_limits<size_t>::max() / 2) {
				newSize = required;
				break;
			}
			newSize <<= 1;
		}
		byte* newMem = new byte[newSize];
		memcpy(newMem, mem, memLen);
		delete[] mem;
		mem = newMem;
		memSize = newSize;
	}
	if (memPos > memLen) {
		memset(mem + memLen, 0, memPos - memLen);
	}
	if (len != 0) {
		memcpy(mem + memPos, in, len);
	}
	memPos+= len;
	if (memPos > memLen) {
		memLen = memPos;
	}
	return false;
}

byte* memOutputStream_c::MemGet()
{
	return mem;
}

bool memOutputStream_c::MemOutput(ioStream_c* out)
{
	return !out || out->Write(mem, memLen);
}

void memOutputStream_c::MemReset()
{
	memLen = memPos = 0;
}

// ===============
// Stdio File Base
// ===============

fileStreamBase_c::fileStreamBase_c()
{
	file = NULL;
}

fileStreamBase_c::~fileStreamBase_c()
{
	FileClose();
}

size_t fileStreamBase_c::GetLen()
{
	if (!file) {
		return 0;
	}
	size_t oldPos = 0;
	size_t size = 0;
	if (!TellFile(file, oldPos) || !SeekFile(file, 0, SEEK_END) || !TellFile(file, size)) {
		return 0;
	}
	// Best effort: callers should still receive the measured length if the
	// underlying stream is non-seekable after the end probe.
	SeekFile(file, oldPos, SEEK_SET);
	return size;
}

size_t fileStreamBase_c::GetPos()
{
	size_t pos = 0;
	return file && TellFile(file, pos) ? pos : 0;
}

bool fileStreamBase_c::Seek(size_t pos, int mode)
{
	if ( !file ) {
		return true;
	}
	return !SeekFile(file, pos, mode);
}

void fileStreamBase_c::FileClose()
{
	if (file) {
		fclose(file);
		file = NULL;
	}
}

// ================
// Stdio File Input
// ================

bool fileInputStream_c::Read(void* out, size_t len)
{
	if (!file || (!out && len != 0)) {
		return true;
	}
	if (len == 0) {
		return false;
	}
	return fread(out, len, 1, file) < 1;
}

bool fileInputStream_c::FileOpen(std::filesystem::path const& fileName, bool binary)
{
	FileClose();
#ifdef _WIN32
	file = _wfopen(fileName.c_str(), binary ? L"rb" : L"r");
#else
	file = fopen(fileName.c_str(), binary ? "rb" : "r");
#endif
	if ( !file ) {
		return true;
	}
	return false;
}

// =================
// Stdio File Output
// =================

bool fileOutputStream_c::Write(const void* in, size_t len)
{
	if (!file || (!in && len != 0)) {
		return true;
	}
	if (len == 0) {
		return false;
	}
	return fwrite(in, len, 1, file) < 1;
}

bool fileOutputStream_c::FileOpen(std::filesystem::path const& fileName, bool binary)
{
	FileClose();
#ifdef _WIN32
	file = _wfopen(fileName.c_str(), binary ? L"wb" : L"w");
#else
	file = fopen(fileName.c_str(), binary ? "wb" : "w");
#endif
	if ( !file ) {
		return true;
	}
	return false;
}

void fileOutputStream_c::FilePrintf(const char* fmt, ...)
{
	if (file) {
		va_list va;
		va_start(va, fmt);
		vfprintf(file, fmt, va);
		va_end(va);
	}
}

void fileOutputStream_c::FileFlush()
{
	if (file) {
		fflush(file);
	}
}