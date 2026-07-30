#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "zlib.h"

#if !defined(_WIN32)
#include <sys/types.h>
#endif

#if defined(_WIN32)
#define LZIP_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define LZIP_EXPORT __attribute__((visibility("default")))
#else
#define LZIP_EXPORT
#endif

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace {

constexpr std::uint32_t kLocalHeaderSignature = 0x04034b50U;
constexpr std::uint16_t kFlagEncrypted = 0x0001U;
constexpr std::uint16_t kFlagDataDescriptor = 0x0008U;
constexpr std::uint16_t kMethodStore = 0;
constexpr std::uint16_t kMethodDeflate = 8;
constexpr std::size_t kInflateBufferSize = 64 * 1024;
constexpr std::size_t kMaxLuaReadBytes = 128 * 1024 * 1024;
constexpr std::size_t kMaxArchiveEntries = 100000;
constexpr std::uint64_t kMaxArchiveUncompressedBytes = 512ULL * 1024ULL * 1024ULL;

std::uint16_t ReadLE16(const std::uint8_t* bytes)
{
	return static_cast<std::uint16_t>(bytes[0]) |
		(static_cast<std::uint16_t>(bytes[1]) << 8);
}

std::uint32_t ReadLE32(const std::uint8_t* bytes)
{
	return static_cast<std::uint32_t>(bytes[0]) |
		(static_cast<std::uint32_t>(bytes[1]) << 8) |
		(static_cast<std::uint32_t>(bytes[2]) << 16) |
		(static_cast<std::uint32_t>(bytes[3]) << 24);
}

bool AddWouldOverflow(std::uint64_t first, std::uint64_t second)
{
	return second > std::numeric_limits<std::uint64_t>::max() - first;
}

bool FileSeek(FILE* file, std::uint64_t offset)
{
#if defined(_WIN32)
	return _fseeki64(file, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
	if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
		return false;
	}
	return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

bool FileSeekToEnd(FILE* file)
{
#if defined(_WIN32)
	return _fseeki64(file, 0, SEEK_END) == 0;
#else
	return fseeko(file, 0, SEEK_END) == 0;
#endif
}

std::optional<std::uint64_t> FileTell(FILE* file)
{
#if defined(_WIN32)
	const __int64 position = _ftelli64(file);
#else
	const off_t position = ftello(file);
#endif
	if (position < 0) {
		return {};
	}
	return static_cast<std::uint64_t>(position);
}

struct fs_fileInfo_s {
	std::string name;
	std::uint64_t dataOffset = 0;
	std::uint32_t uncompressedSize = 0;
	std::uint32_t compressedSize = 0;
	bool deflated = false;
};

class fs_zipFile_c {
public:
	explicit fs_zipFile_c(const char* filename)
	{
		if (!filename || !*filename) {
			return;
		}
		file = std::fopen(filename, "rb");
		if (!file) {
			return;
		}
		ParseLocalEntries();
	}

	~fs_zipFile_c()
	{
		if (file) {
			std::fclose(file);
		}
	}

	fs_zipFile_c(const fs_zipFile_c&) = delete;
	fs_zipFile_c& operator=(const fs_zipFile_c&) = delete;

	bool IsOpen() const
	{
		return file != nullptr;
	}

	bool ReadAt(std::uint64_t offset, std::uint8_t* output, std::size_t length)
	{
		if (!file || (!output && length != 0)) {
			return false;
		}
		std::lock_guard<std::mutex> lock(fileMutex);
		if (!FileSeek(file, offset)) {
			return false;
		}
		return std::fread(output, 1, length, file) == length;
	}

	std::vector<fs_fileInfo_s> files;

private:
	FILE* file = nullptr;
	std::mutex fileMutex;
	std::uint64_t fileSize = 0;

	bool ReadHeaderAt(std::uint64_t offset, std::array<std::uint8_t, 30>& header)
	{
		return offset <= fileSize && header.size() <= fileSize - offset && ReadAt(offset, header.data(), header.size());
	}

	void ParseLocalEntries()
	{
		{
			std::lock_guard<std::mutex> lock(fileMutex);
			if (!FileSeekToEnd(file)) {
				return;
			}
			const auto length = FileTell(file);
			if (!length || !FileSeek(file, 0)) {
				return;
			}
			fileSize = *length;
		}

		std::uint64_t cursor = 0;
		std::uint64_t totalUncompressedBytes = 0;
		while (cursor + 30 <= fileSize) {
			std::array<std::uint8_t, 30> header{};
			if (!ReadHeaderAt(cursor, header) || ReadLE32(header.data()) != kLocalHeaderSignature) {
				break;
			}

			const std::uint16_t flags = ReadLE16(header.data() + 6);
			const std::uint16_t method = ReadLE16(header.data() + 8);
			const std::uint32_t compressedSize = ReadLE32(header.data() + 18);
			const std::uint32_t uncompressedSize = ReadLE32(header.data() + 22);
			const std::uint16_t nameLength = ReadLE16(header.data() + 26);
			const std::uint16_t extraLength = ReadLE16(header.data() + 28);

			const std::uint64_t headerEnd = cursor + header.size();
			if (AddWouldOverflow(headerEnd, nameLength) || AddWouldOverflow(headerEnd + nameLength, extraLength)) {
				break;
			}
			const std::uint64_t dataOffset = headerEnd + nameLength + extraLength;
			if (dataOffset > fileSize || compressedSize > fileSize - dataOffset) {
				break;
			}

			std::string name(nameLength, '\0');
			if (nameLength && !ReadAt(headerEnd, reinterpret_cast<std::uint8_t*>(name.data()), nameLength)) {
				break;
			}

			// Local headers with a trailing data descriptor do not carry reliable
			// sizes.  The historic reader did not parse the central directory, so
			// reject them explicitly rather than seeking into arbitrary data.
			const bool usable = (flags & (kFlagEncrypted | kFlagDataDescriptor)) == 0 &&
				(method == kMethodStore || method == kMethodDeflate) &&
				!name.empty() && name.back() != '/' &&
				uncompressedSize <= static_cast<std::uint32_t>(std::numeric_limits<int>::max()) &&
				compressedSize <= static_cast<std::uint32_t>(std::numeric_limits<int>::max());
			if (usable && files.size() < kMaxArchiveEntries &&
				!AddWouldOverflow(totalUncompressedBytes, uncompressedSize) &&
				totalUncompressedBytes + uncompressedSize <= kMaxArchiveUncompressedBytes) {
				files.push_back({ std::move(name), dataOffset, uncompressedSize, compressedSize, method == kMethodDeflate });
				totalUncompressedBytes += uncompressedSize;
			}
			else if (files.size() >= kMaxArchiveEntries ||
				AddWouldOverflow(totalUncompressedBytes, uncompressedSize) ||
				totalUncompressedBytes + uncompressedSize > kMaxArchiveUncompressedBytes) {
				break;
			}

			const std::uint64_t next = dataOffset + compressedSize;
			if (next <= cursor) {
				break;
			}
			cursor = next;
		}
	}
};

class fs_inflator_c {
public:
	fs_inflator_c(std::shared_ptr<fs_zipFile_c> archive, const fs_fileInfo_s& info)
		: archive(std::move(archive)), info(info), compressedRemaining(info.compressedSize)
	{
		std::memset(&stream, 0, sizeof(stream));
		initialised = inflateInit2(&stream, -MAX_WBITS) == Z_OK;
	}

	~fs_inflator_c()
	{
		if (initialised) {
			inflateEnd(&stream);
		}
	}

	int Inflate(std::uint8_t* output, int length)
	{
		if (!initialised || failed || finished || !output || length <= 0) {
			return 0;
		}

		stream.next_out = output;
		stream.avail_out = static_cast<uInt>(length);
		while (stream.avail_out != 0) {
			if (stream.avail_in == 0 && !FillInput()) {
				if (!finished) {
					failed = true;
				}
				break;
			}
			const uInt beforeIn = stream.avail_in;
			const uInt beforeOut = stream.avail_out;
			const int result = inflate(&stream, Z_NO_FLUSH);
			if (result == Z_STREAM_END) {
				finished = true;
				break;
			}
			if (result != Z_OK) {
				failed = true;
				break;
			}
			if (stream.avail_in == beforeIn && stream.avail_out == beforeOut) {
				failed = true;
				break;
			}
		}
		return length - static_cast<int>(stream.avail_out);
	}

private:
	std::shared_ptr<fs_zipFile_c> archive;
	const fs_fileInfo_s& info;
	std::array<std::uint8_t, kInflateBufferSize> input{};
	z_stream stream{};
	std::uint64_t compressedOffset = 0;
	std::uint64_t compressedRemaining = 0;
	bool initialised = false;
	bool finished = false;
	bool failed = false;

	bool FillInput()
	{
		if (stream.avail_in != 0 || compressedRemaining == 0) {
			return stream.avail_in != 0;
		}
		const std::size_t readSize = static_cast<std::size_t>(std::min<std::uint64_t>(input.size(), compressedRemaining));
		if (!archive->ReadAt(info.dataOffset + compressedOffset, input.data(), readSize)) {
			failed = true;
			return false;
		}
		compressedOffset += readSize;
		compressedRemaining -= readSize;
		stream.next_in = input.data();
		stream.avail_in = static_cast<uInt>(readSize);
		return true;
	}
};

class fs_file_c {
public:
	fs_file_c(std::shared_ptr<fs_zipFile_c> archive, std::size_t fileIndex)
		: archive(std::move(archive)), info(&this->archive->files.at(fileIndex))
	{
		if (info->deflated) {
			inflator = std::make_unique<fs_inflator_c>(this->archive, *info);
		}
	}

	void Seek(int position, int mode)
	{
		std::int64_t target = 0;
		switch (mode) {
		case SEEK_SET: target = position; break;
		case SEEK_CUR: target = static_cast<std::int64_t>(readPosition) + position; break;
		case SEEK_END: target = static_cast<std::int64_t>(info->uncompressedSize) - position; break;
		default: return;
		}
		target = std::clamp<std::int64_t>(target, 0, info->uncompressedSize);

		if (!info->deflated) {
			readPosition = static_cast<std::uint32_t>(target);
			return;
		}
		if (target < readPosition) {
			inflator = std::make_unique<fs_inflator_c>(archive, *info);
			readPosition = 0;
		}
		std::array<std::uint8_t, 4096> discard{};
		while (readPosition < target) {
			const int wanted = static_cast<int>(std::min<std::int64_t>(discard.size(), target - readPosition));
			if (Read(discard.data(), wanted) == 0) {
				break;
			}
		}
	}

	int Read(std::uint8_t* output, int length)
	{
		if (!output || length <= 0 || readPosition >= info->uncompressedSize) {
			return 0;
		}
		const int requested = std::min<int>(length, static_cast<int>(info->uncompressedSize - readPosition));
		int actual = 0;
		if (info->deflated) {
			actual = inflator ? inflator->Inflate(output, requested) : 0;
		}
		else if (archive->ReadAt(info->dataOffset + readPosition, output, requested)) {
			actual = requested;
		}
		readPosition += static_cast<std::uint32_t>(actual);
		return actual;
	}

	int Length() const { return static_cast<int>(info->uncompressedSize); }
	int Tell() const { return static_cast<int>(readPosition); }

private:
	std::shared_ptr<fs_zipFile_c> archive;
	const fs_fileInfo_s* info = nullptr;
	std::uint32_t readPosition = 0;
	std::unique_ptr<fs_inflator_c> inflator;
};

struct lzip_s {
	std::shared_ptr<fs_zipFile_c> zipFile;
};

struct lzipFile_s {
	std::shared_ptr<fs_file_c> file;
};

int IsUserData(lua_State* state, int index, const char* metaName)
{
	if (lua_type(state, index) != LUA_TUSERDATA || lua_getmetatable(state, index) == 0) {
		return 0;
	}
	lua_getfield(state, lua_upvalueindex(1), metaName);
	const int result = lua_rawequal(state, -2, -1);
	lua_pop(state, 2);
	return result;
}

lzip_s* GetZip(lua_State* state, const char* method, bool valid)
{
	if (!IsUserData(state, 1, "zipMeta")) {
		luaL_error(state, "zip:%s() must be used on a zip handle", method);
	}
	auto* zip = static_cast<lzip_s*>(lua_touserdata(state, 1));
	lua_remove(state, 1);
	if (valid && !zip->zipFile) {
		luaL_error(state, "zip:%s(): zip handle is closed", method);
	}
	return zip;
}

lzipFile_s* GetZipFile(lua_State* state, const char* method, bool valid)
{
	if (!IsUserData(state, 1, "zipFileMeta")) {
		luaL_error(state, "zipFile:%s() must be used on a zip file handle", method);
	}
	auto* zipFile = static_cast<lzipFile_s*>(lua_touserdata(state, 1));
	lua_remove(state, 1);
	if (valid && !zipFile->file) {
		luaL_error(state, "zipFile:%s(): zip file handle is closed", method);
	}
	return zipFile;
}

int l_open(lua_State* state)
{
	if (lua_gettop(state) < 1 || !lua_isstring(state, 1)) {
		return luaL_error(state, "Usage: lzip.open(fileName)");
	}
	auto archive = std::make_shared<fs_zipFile_c>(lua_tostring(state, 1));
	if (!archive->IsOpen() || archive->files.empty()) {
		return 0;
	}
	auto* zip = new (lua_newuserdata(state, sizeof(lzip_s))) lzip_s{ std::move(archive) };
	(void)zip;
	lua_pushvalue(state, lua_upvalueindex(2));
	lua_setmetatable(state, -2);
	return 1;
}

int l_zip_Close(lua_State* state)
{
	auto* zip = GetZip(state, "Close", false);
	zip->zipFile.reset();
	return 0;
}

int l_zip_GC(lua_State* state)
{
	auto* zip = static_cast<lzip_s*>(lua_touserdata(state, 1));
	if (zip) {
		zip->~lzip_s();
	}
	return 0;
}

int l_zip_GetNumFiles(lua_State* state)
{
	auto* zip = GetZip(state, "GetNumFiles", true);
	lua_pushinteger(state, static_cast<lua_Integer>(zip->zipFile->files.size()));
	return 1;
}

int l_zip_GetFileName(lua_State* state)
{
	auto* zip = GetZip(state, "GetFileName", true);
	if (lua_gettop(state) < 1 || !lua_isnumber(state, 1)) {
		return luaL_error(state, "Usage: zip:GetFileName(index)");
	}
	const lua_Integer index = lua_tointeger(state, 1);
	if (index < 1 || static_cast<std::size_t>(index) > zip->zipFile->files.size()) {
		return 0;
	}
	lua_pushstring(state, zip->zipFile->files[static_cast<std::size_t>(index - 1)].name.c_str());
	return 1;
}

int l_zip_GetFileSize(lua_State* state)
{
	auto* zip = GetZip(state, "GetFileSize", true);
	if (lua_gettop(state) < 1 || !lua_isnumber(state, 1)) {
		return luaL_error(state, "Usage: zip:GetFileSize(index)");
	}
	const lua_Integer index = lua_tointeger(state, 1);
	if (index < 1 || static_cast<std::size_t>(index) > zip->zipFile->files.size()) {
		return 0;
	}
	lua_pushinteger(state, zip->zipFile->files[static_cast<std::size_t>(index - 1)].uncompressedSize);
	return 1;
}

int l_zip_OpenFile(lua_State* state)
{
	auto* zip = GetZip(state, "OpenFile", true);
	if (lua_gettop(state) < 1 || !(lua_isnumber(state, 1) || lua_isstring(state, 1))) {
		return luaL_error(state, "Usage: zip:OpenFile(index) or zip:OpenFile(fileName)");
	}

	std::size_t index = zip->zipFile->files.size();
	if (lua_isnumber(state, 1)) {
		const lua_Integer inputIndex = lua_tointeger(state, 1);
		if (inputIndex < 1 || static_cast<std::size_t>(inputIndex) > zip->zipFile->files.size()) {
			return luaL_error(state, "zip:OpenFile(): invalid index");
		}
		index = static_cast<std::size_t>(inputIndex - 1);
	}
	else {
		const char* name = lua_tostring(state, 1);
		for (std::size_t candidate = 0; candidate < zip->zipFile->files.size(); ++candidate) {
			if (zip->zipFile->files[candidate].name == name) {
				index = candidate;
				break;
			}
		}
		if (index == zip->zipFile->files.size()) {
			return 0;
		}
	}

	auto file = std::make_shared<fs_file_c>(zip->zipFile, index);
	auto* zipFile = new (lua_newuserdata(state, sizeof(lzipFile_s))) lzipFile_s{ std::move(file) };
	(void)zipFile;
	lua_pushvalue(state, lua_upvalueindex(2));
	lua_setmetatable(state, -2);
	return 1;
}

int l_zipFile_Close(lua_State* state)
{
	auto* zipFile = GetZipFile(state, "Close", false);
	zipFile->file.reset();
	return 0;
}

int l_zipFile_GC(lua_State* state)
{
	auto* zipFile = static_cast<lzipFile_s*>(lua_touserdata(state, 1));
	if (zipFile) {
		zipFile->~lzipFile_s();
	}
	return 0;
}

int l_zipFile_Read(lua_State* state)
{
	auto* zipFile = GetZipFile(state, "Read", true);
	if (lua_gettop(state) < 1) {
		return luaL_error(state, "Usage: zipFile:Read(count) or zipFile:Read(\"*a\")");
	}

	lua_Integer count = 0;
	if (lua_isnumber(state, 1)) {
		count = lua_tointeger(state, 1);
	}
	else if (lua_isstring(state, 1) && std::strcmp(lua_tostring(state, 1), "*a") == 0) {
		count = zipFile->file->Length() - zipFile->file->Tell();
	}
	else {
		return luaL_error(state, "zipFile:Read(): unrecognised format");
	}

	if (count <= 0) {
		lua_pushliteral(state, "");
		return 1;
	}
	if (static_cast<std::uint64_t>(count) > kMaxLuaReadBytes) {
		return luaL_error(state, "zipFile:Read(): requested read exceeds the %u MiB safety limit", static_cast<unsigned>(kMaxLuaReadBytes / (1024 * 1024)));
	}
	std::vector<std::uint8_t> buffer(static_cast<std::size_t>(count));
	const int actual = zipFile->file->Read(buffer.data(), static_cast<int>(count));
	lua_pushlstring(state, reinterpret_cast<const char*>(buffer.data()), static_cast<std::size_t>(actual));
	return 1;
}

int l_zipFile_Length(lua_State* state)
{
	auto* zipFile = GetZipFile(state, "Length", true);
	lua_pushinteger(state, zipFile->file->Length());
	return 1;
}

} // namespace

extern "C" LZIP_EXPORT int luaopen_lzip(lua_State* state)
{
	lua_settop(state, 0);
	lua_newtable(state); // library table
	lua_newtable(state); // zip metatable
	lua_newtable(state); // zip file metatable

	lua_pushvalue(state, 2);
	lua_setfield(state, 1, "zipMeta");
	lua_pushvalue(state, 2);
	lua_setfield(state, 2, "__index");

	lua_pushvalue(state, 3);
	lua_setfield(state, 1, "zipFileMeta");
	lua_pushvalue(state, 3);
	lua_setfield(state, 3, "__index");

	lua_pushvalue(state, 1);
	lua_pushvalue(state, 2);
	lua_pushcclosure(state, l_open, 2);
	lua_setfield(state, 1, "open");

	lua_pushcfunction(state, l_zip_GC);
	lua_setfield(state, 2, "__gc");
	lua_pushvalue(state, 1);
	lua_pushcclosure(state, l_zip_Close, 1);
	lua_setfield(state, 2, "Close");
	lua_pushvalue(state, 1);
	lua_pushcclosure(state, l_zip_GetNumFiles, 1);
	lua_setfield(state, 2, "GetNumFiles");
	lua_pushvalue(state, 1);
	lua_pushcclosure(state, l_zip_GetFileName, 1);
	lua_setfield(state, 2, "GetFileName");
	lua_pushvalue(state, 1);
	lua_pushcclosure(state, l_zip_GetFileSize, 1);
	lua_setfield(state, 2, "GetFileSize");
	lua_pushvalue(state, 1);
	lua_pushvalue(state, 3);
	lua_pushcclosure(state, l_zip_OpenFile, 2);
	lua_setfield(state, 2, "OpenFile");

	lua_pushcfunction(state, l_zipFile_GC);
	lua_setfield(state, 3, "__gc");
	lua_pushvalue(state, 1);
	lua_pushcclosure(state, l_zipFile_Close, 1);
	lua_setfield(state, 3, "Close");
	lua_pushvalue(state, 1);
	lua_pushcclosure(state, l_zipFile_Read, 1);
	lua_setfield(state, 3, "Read");
	lua_pushvalue(state, 1);
	lua_pushcclosure(state, l_zipFile_Length, 1);
	lua_setfield(state, 3, "Length");

	lua_pop(state, 2);
	return 1;
}
