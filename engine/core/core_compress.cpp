#include "core_compress.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace {

constexpr size_t kMaxDecompressedZstdBytes = 512U * 1024U * 1024U;

}

std::optional<std::vector<char>> CompressZstandard(gsl::span<const std::byte> src, std::optional<int> level)
{
	if (!level)
		level = ZSTD_defaultCLevel();
	std::vector<char> dst(ZSTD_compressBound(src.size()));
	size_t rc = ZSTD_compress(dst.data(), dst.size(), src.data(), src.size(), *level);
	if (ZSTD_isError(rc))
		return {};
	dst.resize(rc);
	return dst;
}

std::optional<std::vector<char>> DecompressZstandard(gsl::span<const std::byte> src)
{
	if (src.empty()) {
		return {};
	}

	const unsigned long long declaredSize = ZSTD_getFrameContentSize(src.data(), src.size());
	if (declaredSize == ZSTD_CONTENTSIZE_ERROR ||
		(declaredSize != ZSTD_CONTENTSIZE_UNKNOWN && declaredSize > kMaxDecompressedZstdBytes)) {
		return {};
	}

	const size_t buffOutSize = ZSTD_DStreamOutSize();
	std::vector<char> buffOut(buffOutSize);

	std::vector<char> dst;
	dst.reserve((std::min)(kMaxDecompressedZstdBytes, declaredSize == ZSTD_CONTENTSIZE_UNKNOWN
		? size_t{ 1 } << 20
		: static_cast<size_t>(declaredSize)));

	ZSTD_DCtx* dctx = ZSTD_createDCtx();
	if (!dctx) {
		return {};
	}
	ZSTD_inBuffer input = { src.data(), src.size(), 0 };
	bool finishedFrame = false;
	while (input.pos < input.size) {
		ZSTD_outBuffer output = { buffOut.data(), buffOut.size(), 0 };
		const size_t rc = ZSTD_decompressStream(dctx, &output, &input);
		if (ZSTD_isError(rc)) {
			ZSTD_freeDCtx(dctx);
			return {};
		}
		auto oldSize = dst.size();
		if (output.pos > kMaxDecompressedZstdBytes - oldSize) {
			ZSTD_freeDCtx(dctx);
			return {};
		}
		auto newSize = oldSize + output.pos;
		if (newSize > dst.capacity()) {
			const size_t grown = newSize > kMaxDecompressedZstdBytes / 2
				? kMaxDecompressedZstdBytes
				: (std::min)(kMaxDecompressedZstdBytes, newSize * 2);
			dst.reserve(grown);
		}
		dst.resize(newSize);
		memcpy(dst.data() + oldSize, output.dst, output.pos);
		finishedFrame = rc == 0;
	}
	ZSTD_freeDCtx(dctx);
	if (!finishedFrame) {
		return {};
	}

	dst.shrink_to_fit();
	return dst;
}
