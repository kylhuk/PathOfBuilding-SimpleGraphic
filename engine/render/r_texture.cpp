// SimpleGraphic Engine
// (c) David Gowor, 2014
//
// Module: Render Texture
//

#include <atomic>
#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>
#include "r_local.h"

#include "cmp_core.h"
#include "stb_image_resize.h"
#include <gli/gl.hpp>
#include <gli/generate_mipmaps.hpp>

// ===================
// Predefined textures
// ===================

static const byte t_whiteImage[64] = {
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

static const byte t_blackImage[64 * 4] = {};

static const byte t_defaultTexture[64] = {
	0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F,
	0x7F, 0x7F, 0x7F, 0x00, 0x00, 0x00, 0x7F, 0x7F,
	0x7F, 0x7F, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x7F,
	0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F,
	0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F,
	0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F,
	0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F,
	0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F
};

static std::unique_ptr<image_c> MakeFallbackTextureImage()
{
	auto image = std::make_unique<image_c>();
	if (!image->CopyRaw(IMGTYPE_GRAY, 8, 8, t_defaultTexture)) {
		return {};
	}
	return image;
}

// =======================
// r_ITexManager Interface
// =======================

class t_manager_c: public r_ITexManager, public thread_c {
public:
	// Interface
	int		GetAsyncCount() override;
	void	ProcessPendingTextureUploads() override;

	// Encapsulated
	t_manager_c(r_renderer_c* renderer);
	~t_manager_c();

	r_renderer_c* renderer;

	r_tex_c* whiteTex;
	r_tex_c* blackTex;

	bool	AsyncAdd(r_tex_c* tex);
	bool	AsyncRemove(r_tex_c* tex);

	void	EnqueueTextureUpload(r_tex_c* tex);
	void	RemovePendingTextureUpload(r_tex_c* tex);

private:
	std::atomic<bool> doRun;
	std::atomic<int> runnersRunning;

	std::vector<std::thread> workers;
	std::vector<r_tex_c *> textureQueue;
	std::mutex mutex;
	std::condition_variable queueCondition;
	std::condition_variable stateCondition;

	std::vector<r_tex_c *> uploadQueue;

	void	ThreadProc() override;
};

r_ITexManager* r_ITexManager::GetHandle(r_renderer_c* renderer)
{
	return new t_manager_c(renderer);
}

void r_ITexManager::FreeHandle(r_ITexManager* hnd)
{
	delete (t_manager_c*)hnd;
}

t_manager_c::t_manager_c(r_renderer_c* renderer)
	: thread_c(renderer->sys), renderer(renderer)
{
	whiteTex = new r_tex_c(this, "@white", 0);
	blackTex = new r_tex_c(this, "@black", 0);

	doRun = true;
	runnersRunning = 0;
	const auto hardwareThreads = std::thread::hardware_concurrency();
	const int runnersWanted = std::clamp(static_cast<int>(hardwareThreads ? hardwareThreads / 2 : 1), 1, 4);

	for (int i = 0; i < runnersWanted; ++i)
	{
		workers.emplace_back([this] {
			ThreadProc();
			});
	}
	while (runnersRunning < runnersWanted) {
		renderer->sys->Sleep(1);
	}
}

t_manager_c::~t_manager_c()
{
	doRun = false;
	queueCondition.notify_all();
	for (auto& worker : workers)
		worker.join();
	textureQueue.clear();

	delete whiteTex;
	delete blackTex;
}

// =====================
// Texture Manager Class
// =====================

int t_manager_c::GetAsyncCount()
{
	std::lock_guard<std::mutex> lock ( mutex );
	return static_cast<int>(textureQueue.size() + uploadQueue.size());
}

void t_manager_c::ProcessPendingTextureUploads()
{
	for (;;) {
		r_tex_c* tex = nullptr;
		{
			std::unique_lock<std::mutex> lock(mutex);
			if (uploadQueue.empty()) {
				return;
			}
			tex = uploadQueue.back();
			uploadQueue.pop_back();
			if (tex->status != r_tex_c::PENDING_UPLOAD) {
				continue;
			}
			tex->status = r_tex_c::UPLOADING;
		}

		r_tex_c::PerformUpload(tex);

		// AsyncRemove waits for all ownership transitions under this mutex.
		// The texture is now terminal (DONE or INIT) and therefore safe to
		// destroy once a waiter has observed the notification.
		{
			std::lock_guard<std::mutex> lock(mutex);
			stateCondition.notify_all();
		}
	}
}
bool t_manager_c::AsyncAdd(r_tex_c* tex)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (runnersRunning == 0 || !doRun) {
		return true;
	}
	textureQueue.push_back(tex);
	tex->status = r_tex_c::IN_QUEUE;
	queueCondition.notify_one();
	return false;
}

bool t_manager_c::AsyncRemove(r_tex_c* tex)
{
	std::unique_lock<std::mutex> lock(mutex);
	if (tex->status == r_tex_c::IN_QUEUE) {
		for (auto itr = textureQueue.begin(); itr != textureQueue.end(); ++itr) {
			if (*itr == tex) {
				textureQueue.erase(itr);
				tex->status = r_tex_c::INIT;
				stateCondition.notify_all();
				return true;
			}
		}
	}

	stateCondition.wait(lock, [tex] {
		const auto status = tex->status.load();
		return status != r_tex_c::PROCESSING
			&& status != r_tex_c::SIZE_KNOWN
			&& status != r_tex_c::UPLOADING;
	});

	if (tex->status == r_tex_c::PENDING_UPLOAD) {
		if (auto I = std::find(uploadQueue.begin(), uploadQueue.end(), tex); I != uploadQueue.end()) {
			uploadQueue.erase(I);
			tex->status = r_tex_c::INIT;
			stateCondition.notify_all();
		}
	}

	return false;
}

void t_manager_c::EnqueueTextureUpload(r_tex_c* tex)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (tex->status == r_tex_c::INIT
		|| tex->status == r_tex_c::PROCESSING
		|| tex->status == r_tex_c::SIZE_KNOWN) {
		uploadQueue.push_back(tex);
		tex->status = r_tex_c::PENDING_UPLOAD;
		stateCondition.notify_all();
	}
}

void t_manager_c::RemovePendingTextureUpload(r_tex_c* tex)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (auto I = std::find(uploadQueue.begin(), uploadQueue.end(), tex); I != uploadQueue.end()) {
		uploadQueue.erase(I);
		tex->status = r_tex_c::INIT;
		stateCondition.notify_all();
	}
}

void t_manager_c::ThreadProc()
{
	++runnersRunning;
	while (doRun) {
		r_tex_c *doTex = nullptr;
		{
			std::unique_lock<std::mutex> lock(mutex);
			queueCondition.wait(lock, [this] { return !doRun || !textureQueue.empty(); });
			if (!doRun) {
				break;
			}

			// Find a texture with the highest loading priority
			int maxPri = 0;
			auto doTexItr = textureQueue.end();
			for (auto curTexItr = textureQueue.begin(); curTexItr != textureQueue.end(); ++curTexItr) {
				auto curTex = *curTexItr;
				if (doTexItr == textureQueue.end() || curTex->loadPri > maxPri) {
					maxPri = curTex->loadPri;
					doTexItr = curTexItr;
				}
			}

			if (doTexItr != textureQueue.end()) {
				doTex = *doTexItr;
				textureQueue.erase(doTexItr);
				doTex->status = r_tex_c::PROCESSING;
			}
		}

		if (doTex != nullptr) {
			// Load this texture. A malformed file or allocation failure must not
			// let an exception escape a worker thread and terminate the process.
			try {
				doTex->LoadFile();
			}
			catch (std::exception const&) {
				doTex->error.store(1, std::memory_order_relaxed);
				doTex->img = MakeFallbackTextureImage();
				EnqueueTextureUpload(doTex);
			}
			catch (...) {
				doTex->error.store(1, std::memory_order_relaxed);
				doTex->img = MakeFallbackTextureImage();
				EnqueueTextureUpload(doTex);
			}
			doTex = nullptr;
		}
	}
	--runnersRunning;
	stateCondition.notify_all();
}

// ====================
// OpenGL Texture Class
// ====================

r_tex_c::r_tex_c(r_ITexManager* manager, std::string_view fileName, int flags)
{
	Init(manager, fileName, flags);

	StartLoad();
	if (status == INIT) {
		// Load it now
		LoadFile();
	}
}

r_tex_c::r_tex_c(r_ITexManager* manager, std::unique_ptr<image_c> img, int flags)
{
	Init(manager, {}, flags);

	// Direct upload
	this->img = BuildMipSet(std::move(img));
	if (!this->img) {
		error.store(1, std::memory_order_relaxed);
		this->img = MakeFallbackTextureImage();
	}
	if (this->img) {
		PerformUpload(this);
	}
}

r_tex_c::~r_tex_c()
{
	if (status >= IN_QUEUE && status < DONE) {
		manager->AsyncRemove(this);
	}
	glDeleteTextures(1, &texId);
}

void r_tex_c::Init(r_ITexManager* i_manager, std::string_view i_fileName, int i_flags)
{
	manager = (t_manager_c*)i_manager;
	renderer = manager->renderer;
	error.store(0, std::memory_order_relaxed);
	status = INIT;
	loadPri = 0;
	texId = 0;
	flags = i_flags;
	fileName = i_fileName;
	fileWidth = 0;
	fileHeight = 0;
}

void r_tex_c::Bind()
{
	if (status == DONE) {
		glBindTexture(target, texId);
	} else {
		manager->blackTex->Bind();
	}
}

void r_tex_c::Unbind()
{
	glBindTexture(target, 0);
}

void r_tex_c::Enable()
{
	glEnable(GL_TEXTURE_2D);
}

void r_tex_c::Disable()
{
	Unbind();
	glDisable(GL_TEXTURE_2D);
}

void r_tex_c::StartLoad()
{
	if (flags & TF_ASYNC)
		manager->AsyncAdd(this);
}

void r_tex_c::AbortLoad()
{
	manager->AsyncRemove(this);
}

void r_tex_c::ForceLoad()
{
	if (status == INIT) {
		LoadFile();
	}
	else if (status == IN_QUEUE && manager->AsyncRemove(this)) {
		// We own it again after removing it from the worker queue.  Never load
		// the same texture on the render and worker threads concurrently.
		LoadFile();
	}
}

std::unique_ptr<image_c> r_tex_c::BuildMipSet(std::unique_ptr<image_c> img)
{
	if (!img) {
		return {};
	}

	const auto format = img->tex.format();

	const bool blockCompressed = is_compressed(format);
	const auto maxDim = (std::max)(1, static_cast<int>(renderer->texMaxDim));
	auto numLevels = img->tex.levels();

	const auto shrinksNeeded = [&t = img->tex, maxDim] {
		auto extent = t.extent();
		int shrinks = 0;
		for (; extent.x > maxDim || extent.y > maxDim; ++shrinks) {
			extent.x = (std::max)(1, extent.x / 2);
			extent.y = (std::max)(1, extent.y / 2);
		}
		return shrinks;
		}();

	// There is an invariant we need to maintain here of that no sides may exceed the maximum dimensions of the renderer.
	// For block-compressed textures we could drop finer mips until we reach a coarser level that's sufficiently reduced in size.
	// We can't generate additional levels for those unless we pull in block format decoding via something like Compressonator
	// and then we would have to consider whether to upload recompressed or burn VRAM on a non-compressed texture.
	// For regular textures we can resize proportionally down for the largest axis to reach the max dimension.

	if (shrinksNeeded) {
		if (shrinksNeeded < static_cast<int>(numLevels)) {
			auto& t = img->tex;
			t = gli::texture2d_array(t,
				t.base_layer(), t.max_layer(),
				t.base_level() + shrinksNeeded, t.max_level());
		}
		else {
			// Compressed formats cannot be resized without a decoder.  Returning
			// no image makes the caller use the small fallback texture instead of
			// issuing an oversized GL allocation.
			if (blockCompressed) {
				return {};
			}

			const int components = static_cast<int>(gli::component_count(format));
			// stb's uint8 resampler is only valid for tightly packed 8-bit
			// channel data; reject float, integer, and packed formats here.
			if ((components != 1 && components != 3 && components != 4)
				|| gli::block_size(format) != static_cast<size_t>(components)) {
				return {};
			}
			const auto sourceExtent = img->tex.extent(0);
			if (sourceExtent.x <= 0 || sourceExtent.y <= 0) {
				return {};
			}
			const double scale = (std::min)(1.0,
				(std::min)(static_cast<double>(maxDim) / sourceExtent.x,
					static_cast<double>(maxDim) / sourceExtent.y));
			const glm::ivec2 resizedExtent{
				(std::max)(1, static_cast<int>(std::floor(sourceExtent.x * scale))),
				(std::max)(1, static_cast<int>(std::floor(sourceExtent.y * scale)))
			};
			auto resized = gli::texture2d_array(format, resizedExtent, img->tex.layers(), 1, img->tex.swizzles());
			const int alphaChannel = components == 4 ? 3 : STBIR_ALPHA_CHANNEL_NONE;
			for (size_t layer = 0; layer < img->tex.layers(); ++layer) {
				if (!stbir_resize_uint8_srgb_edgemode(
					img->tex.data<uint8_t>(layer, 0, 0), sourceExtent.x, sourceExtent.y, sourceExtent.x * components,
					resized.data<uint8_t>(layer, 0, 0), resizedExtent.x, resizedExtent.y, resizedExtent.x * components,
					components, alphaChannel, 0, STBIR_EDGE_CLAMP)) {
					return {};
				}
			}
			img->tex = std::move(resized);
		}
	}

	const bool generateMips = !blockCompressed && img->tex.levels() == 1 && !(flags & TF_NOMIPMAP);

	if (generateMips) {
		if (blockCompressed) {
			// TODO(LV): Mipmap generation requested for a block-compressed texture. This requires decompression.
		}
		else {
			const auto format = img->tex.format();
			const auto extent = img->tex.extent();
			const auto layers = img->tex.layers();
			const auto swizzles = img->tex.swizzles();
			auto newTex = gli::texture2d_array(format, extent, layers, swizzles);
			for (size_t layer = 0; layer < layers; ++layer) {
				newTex.copy(img->tex, layer, 0, 0, layer, 0, 0);
				const size_t levels = newTex.levels();
				for (size_t level = 1; level < levels; ++level) {
					const auto srcExtent = newTex.extent(level - 1);
					const auto comp = (int)gli::component_count(format);
					const auto dstExtent = newTex.extent(level);
					const bool hasAlpha = comp == 4;
					stbir_resize_uint8_srgb_edgemode(
						newTex.data<uint8_t>(layer, 0, level - 1), srcExtent.x, srcExtent.y, srcExtent.x * comp,
						newTex.data<uint8_t>(layer, 0, level), dstExtent.x, dstExtent.y, dstExtent.x * comp,
						comp, hasAlpha ? 3 : STBIR_ALPHA_CHANNEL_NONE, 0, STBIR_EDGE_CLAMP);
				}
			}
			//newTex = gli::generate_mipmaps(newTex, gli::FILTER_LINEAR);
			img->tex = newTex;
		}
	}
	return img;
}

static gli::texture2d_array TranscodeTexture(gli::texture2d_array src, gli::format dstFormat, bool dropFinestMipIfPossible)
{
	// Very limited format support, only really sufficient as a fallback when BC7 isn't available.

	// Source formats: BC7
	const auto srcFormat = src.format();
	if (src.format() != gli::FORMAT_RGBA_BP_UNORM_BLOCK16)
		return src;

	// Destination formats: BC3 or RGBA8
	if (dstFormat != gli::FORMAT_RGBA_DXT5_UNORM_BLOCK16 && dstFormat != gli::FORMAT_RGBA8_UNORM_PACK8)
		return src;

	// To save VRAM and processing costs, there is the option to discard the finest mip level of the source if there's coarser levels available.
	// If so, the transcoding will generate destination levels 0..n-1 from levels 1..n of the source.
	size_t firstLevel = 0;
	if (dropFinestMipIfPossible && src.levels() > 1)
		firstLevel = 1;

	const auto outExtent = src.extent(firstLevel);
	const auto outLayers = src.layers();
	const auto outLevels = src.levels() - firstLevel;

	gli::texture2d_array dst(dstFormat, outExtent, outLayers, outLevels);

	std::array<uint8_t, 64> rgba{};
	for (size_t layer = 0; layer < outLayers; ++layer) {
		for (size_t dstLevel = 0; dstLevel < outLevels; ++dstLevel) {
			auto* dstData = (uint8_t*)dst.data(layer, 0, dstLevel);
			const auto dstExtent = dst.extent(dstLevel);
			const auto dstRowStride = dstExtent.x * 4;

			const size_t srcLevel = dstLevel + firstLevel;
			const auto* srcData = (const uint8_t*)src.data(layer, 0, srcLevel);

			const auto srcBlockSize = gli::block_extent(srcFormat);
			const auto srcBlocksPerRow = (dstExtent.y + srcBlockSize.y - 1) / srcBlockSize.y; // round up partial blocks
			const auto srcBlocksPerColumn = (dstExtent.x + srcBlockSize.x - 1) / srcBlockSize.x; // -''-

			for (size_t blockRow = 0; blockRow < srcBlocksPerRow; ++blockRow) {
				const size_t rowBase = blockRow * srcBlockSize.y;
				const size_t rowsLeft = (std::min)(size_t{ 4 }, dstExtent.y - rowBase);

				for (size_t blockCol = 0; blockCol < srcBlocksPerColumn; ++blockCol) {
					// Read source 4x4 texel block, no branching needed.
					DecompressBlockBC7(srcData, rgba.data());

					// Recompress or distribute the 4x4 RGBA block.
					if (dstFormat == gli::FORMAT_RGBA_DXT5_UNORM_BLOCK16) {
						// The block order in the level data for BC3 is the same as for BC7, so we can just append them as they appear.
						CompressBlockBC3(rgba.data(), 16, dstData + blockCol * gli::block_size(dstFormat));

						// Advance the storage write pointer as we go.
						dstData += gli::block_size(dstFormat);
					}
					else if (dstFormat == gli::FORMAT_RGBA8_UNORM_PACK8) {
						// Compressed blocks unconditionally have 4x4 texels each, even if the source extent isn't evenly divisible into blocks with padding on the right and bottom of the block.
						// When copying these to RGBA storage which doesn't have this padding we need to ensure we don't go past the edges of the destination.

						// Here we work off that dstData points at the top left pixel of the block row in the destination.
						const size_t colBase = blockCol * srcBlockSize.x;
					const size_t colsLeft = (std::min)(size_t{ 4 }, dstExtent.x - colBase);
						const size_t colBytesLeft = colsLeft * 4;
						for (size_t innerRow = 0; innerRow < rowsLeft; ++innerRow) {
							auto* dstPtr = dstData + dstRowStride * innerRow + colBase * 4;
							memcpy(dstPtr, rgba.data() + innerRow * 16, colBytesLeft);
						}
						// Note that dstData is advanced at the end of the source block row to make copy logic easier to follow.
					}
					srcData += gli::block_size(srcFormat);
				}

				// Advance the destination buffer only at the end of an source block row if writing to RGBA output.
				if (!gli::is_compressed(dstFormat))
					dstData += dstRowStride * rowsLeft;
			}

			// Both source and destination cursors advance as blocks are
			// transcoded. Their starting addresses are level-relative, so a
			// post-loop assertion against the already advanced pointer would
			// always be false in debug builds.
		}
	}

	return dst;
}

void r_tex_c::LoadFile()
{
	auto queueUpload = [this] {
		if (flags & TF_ASYNC) {
			// Worker threads only decode data; OpenGL uploads remain on the
			// render thread.
			manager->EnqueueTextureUpload(this);
		}
		else {
			status = PENDING_UPLOAD;
			PerformUpload(this);
		}
	};

	if (_stricmp(fileName.c_str(), "@white") == 0) {
		img = std::make_unique<image_c>();
		img->CopyRaw(IMGTYPE_GRAY, 8, 8, t_whiteImage);
		queueUpload();
		return;
	}
	else if (_stricmp(fileName.c_str(), "@black") == 0) {
		img = std::make_unique<image_c>();
		img->CopyRaw(IMGTYPE_RGBA, 8, 8, t_blackImage);
		queueUpload();
		return;
	}

	// Try to load image file using appropriate loader
	auto path = std::filesystem::u8path(fileName);
	img = std::unique_ptr<image_c>(image_c::LoaderForFile(renderer->sys->con, path));
	if (img) {
		auto sizeCallback = [this](int width, int height) {
			this->fileWidth = width;
			this->fileHeight = height;
			this->status = SIZE_KNOWN;
		};
			error.store(img->Load(path, sizeCallback), std::memory_order_relaxed);
		if (error.load(std::memory_order_relaxed) == 0) {
			const bool useTextureFormatFallback = !renderer->texBC7;
			if (useTextureFormatFallback) {
				if (img->tex.format() == gli::FORMAT_RGBA_BP_UNORM_BLOCK16)
					img->tex = TranscodeTexture(img->tex, gli::FORMAT_RGBA8_UNORM_PACK8, true);
			}
			img = BuildMipSet(std::move(img));
			if (img) {
				stackLayers.store(img->tex.layers(), std::memory_order_relaxed);
				queueUpload();
				return;
			}
			error.store(1, std::memory_order_relaxed);
		}
	}

	stackLayers.store(1, std::memory_order_relaxed);
	img = MakeFallbackTextureImage();
	queueUpload();
}

void r_tex_c::PerformUpload(r_tex_c* tex)
{
	if (!tex) {
		return;
	}
	if (!tex->img) {
		tex->status = INIT;
		return;
	}
	tex->Upload(*tex->img, tex->flags);
	tex->img = {};
	tex->status = DONE;
}

void r_tex_c::Upload(image_c& img, int flags)
{
	static gli::gl gl(gli::gl::PROFILE_ES30);

	const auto& tex = img.tex;
	target = gl.translate(tex.target());
	const auto format = gl.translate(tex.format(), tex.swizzles());

	// Find and bind texture name
	glGenTextures(1, &texId);
	glBindTexture(target, texId);

	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(tex.levels() - 1));
	glTexParameteri(target, GL_TEXTURE_SWIZZLE_R, format.Swizzles.r);
	glTexParameteri(target, GL_TEXTURE_SWIZZLE_G, format.Swizzles.g);
	glTexParameteri(target, GL_TEXTURE_SWIZZLE_B, format.Swizzles.b);
	glTexParameteri(target, GL_TEXTURE_SWIZZLE_A, format.Swizzles.a);

	const int miplevels = (int)tex.levels();

	// Set filters
	if (miplevels == 1) {
		glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	}
	else {
		glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	}
	if (flags & TF_NEAREST) {
		glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	else {
		glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}

	constexpr float anisotropyCap = 16.0f;
	static const bool anisotropySupported = [] {
		char const* extensions = reinterpret_cast<char const*>(glGetString(GL_EXTENSIONS));
		return extensions && strstr(extensions, "GL_EXT_texture_filter_anisotropic");
		}();
	if (anisotropySupported) {
		float maxAnisotropy = 1.0f;
		glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAnisotropy);
		glTexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY,
			(std::max)(1.0f, (std::min)(maxAnisotropy, anisotropyCap)));
	}

	// Set repeating
	if (flags & TF_CLAMP) {
		glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	else {
		glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_REPEAT);
	}

	const int layers = (int)tex.layers();
	const auto extent = tex.extent();
	const bool isTextureArray = target == GL_TEXTURE_2D_ARRAY;

	if (isTextureArray)
		glTexStorage3D(target, miplevels, format.Internal, extent.x, extent.y, layers);
	else
		glTexStorage2D(target, miplevels, format.Internal, extent.x, extent.y);

	for (int layer = 0; layer < layers; ++layer) {
		for (int miplevel = 0; miplevel < miplevels; ++miplevel) {

			const auto extent = tex.extent(miplevel);

			const int up_w = extent.x;
			const int up_h = extent.y;

			// Upload the mipmap
			const auto* data = tex.data(layer, 0, miplevel);
			if (is_compressed(tex.format()))
				if (isTextureArray)
					glCompressedTexSubImage3D(target, miplevel, 0, 0, layer, extent.x, extent.y, 1, format.Internal, (GLsizei)tex.size(miplevel), data);
				else
					glCompressedTexSubImage2D(target, miplevel, 0, 0, extent.x, extent.y, format.Internal, (GLsizei)tex.size(miplevel), data);
			else
				if (isTextureArray)
					glTexSubImage3D(target, miplevel, 0, 0, layer, extent.x, extent.y, 1, format.External, format.Type, data);
				else
					glTexSubImage2D(target, miplevel, 0, 0, extent.x, extent.y, format.External, format.Type, data);
		}
	}
}
