// basis_wrappers.cpp - Wrappers to the C++ compressor and transcoder for WebAssembly/WebGL use.
//
// Important: 
// Compile with -fno-strict-aliasing
// The "initializeBasis()" function MUST be called at least once before using either the compressor or transcoder.
//
// There are four main categories of wrappers in this module:
// 1. Transcoding, low-level .basis file information: See class basis_file. Only depends on transcoder/basisu_transcoder.cpp.
//    getFileDesc(), getImageDesc(), getImageLevelDesc(): These functions return low-level information about where compressed data is located for each image in a .basis file.
//    This is useful for when you want to extract the compressed data and embed it into your own file formats, for container independent transcoding.
//
// 2. Encoding (optional): See class basis_encoder. Encodes PNG or 32bpp images to .basis files in memory. Must compile with BASISU_SUPPORT_ENCODING=1. 
//    Requires basisu_transcoder.cpp as well as all the .cpp files in the "encoder" directory. Results in a larger WebAssembly executable.
//
// 3. Low level transcoding/container independent transcoding: See class lowlevel_etc1s_image_transcoder or function transcodeUASTCImage(). For
//    transcoding raw compressed ETC1S/UASTC texture data from non-.basis files (say from KTX2) to GPU texture data.
//
// 4. Helpers, transcoder texture format information: See functions getBytesPerBlockOrPixel(), formatHasAlpha(), etc.

// If BASISU_SUPPORT_ENCODING is 1, wrappers for the compressor will be included. Otherwise, only the wrappers for the transcoder will be compiled in.
#ifndef BASISU_SUPPORT_ENCODING
#define BASISU_SUPPORT_ENCODING 0
#endif

// Enable debug printf()'s in this module.
#ifndef BASISU_DEBUG_PRINTF
#define BASISU_DEBUG_PRINTF 0
#endif

#include "basisu_transcoder.h"
#include <emscripten/bind.h>
#include <algorithm>

#if BASISU_SUPPORT_ENCODING
#include "../../encoder/basisu_comp.h"
#include "../../encoder/basisu_resampler_filters.h"
#endif

using namespace emscripten;
using namespace basist;
using namespace basisu;

static basist::etc1_global_selector_codebook* g_pGlobal_codebook;

void basis_init()
{
#if BASISU_DEBUG_PRINTF
	printf("basis_init()\n");
#endif   

	if (g_pGlobal_codebook)
		return;

#if BASISU_SUPPORT_ENCODING
	basisu_encoder_init();
#endif

	basisu_transcoder_init();

	g_pGlobal_codebook = new basist::etc1_global_selector_codebook(g_global_selector_cb_size, g_global_selector_cb);
}

static void copy_from_jsbuffer(const emscripten::val& srcBuffer, std::vector<uint8_t>& dstVec)
{
	unsigned int length = srcBuffer["length"].as<unsigned int>();

	dstVec.resize(length);

	emscripten::val memory = emscripten::val::module_property("HEAP8")["buffer"];
	emscripten::val memoryView = srcBuffer["constructor"].new_(memory, reinterpret_cast<uintptr_t>(dstVec.data()), length);
	
	// Copy the bytes from the Javascript buffer.
	memoryView.call<void>("set", srcBuffer);
}

static bool copy_to_jsbuffer(const emscripten::val& dstBuffer, const std::vector<uint8_t>& srcVec)
{
	if (srcVec.empty())
	{
#if BASISU_DEBUG_PRINTF	
		printf("copy_to_jsbuffer: Provided source buffer is empty\n");
#endif
		return false;
	}
		
	// Make sure the provided buffer from Javascript is big enough. If not, bail.
	int dstBufferLen = dstBuffer["byteLength"].as<int>();
			
	if (srcVec.size() > dstBufferLen)
	{
#if BASISU_DEBUG_PRINTF	
		printf("copy_to_jsbuffer: Provided destination buffer is too small (wanted %u bytes, got %u bytes)!\n", (uint32_t)srcVec.size(), dstBufferLen);
#endif
		assert(0);
		return false;
	}

	emscripten::val memory = emscripten::val::module_property("HEAP8")["buffer"];
	emscripten::val memoryView = emscripten::val::global("Uint8Array").new_(memory, reinterpret_cast<uintptr_t>(srcVec.data()), srcVec.size());

	// Copy the bytes into the Javascript buffer.
	dstBuffer.call<void>("set", memoryView);
	
	return true;
}

#define MAGIC 0xDEADBEE1

struct basis_file_desc
{
	uint32_t m_version;
	
	uint32_t m_us_per_frame;
	
	uint32_t m_total_images;
	
	uint32_t m_userdata0;
	uint32_t m_userdata1;
	
	// Type of texture (cETC1S or cUASTC4x4)
	uint32_t m_tex_format; // basis_tex_format 
	
	bool m_y_flipped;
	bool m_has_alpha_slices;
		
	// ETC1S endpoint codebook
	uint32_t m_num_endpoints;
	uint32_t m_endpoint_palette_ofs;
	uint32_t m_endpoint_palette_len;

	// ETC1S selector codebook
	uint32_t m_num_selectors;
	uint32_t m_selector_palette_ofs;
	uint32_t m_selector_palette_len;

	// Huffman codelength tables	
	uint32_t m_tables_ofs;
	uint32_t m_tables_len;
};

struct basis_image_desc
{
	uint32_t m_orig_width;
	uint32_t m_orig_height;
	uint32_t m_num_blocks_x;
	uint32_t m_num_blocks_y;
	uint32_t m_num_levels;

	// Will be true if the image has alpha (for UASTC this may vary per-image)
	bool m_alpha_flag;	
	bool m_iframe_flag;
};

struct basis_image_level_desc
{
	// File offset/length of the compressed ETC1S or UASTC texture data.
	uint32_t m_rgb_file_ofs;
	uint32_t m_rgb_file_len;

	// Optional alpha data file offset/length - will be 0's for UASTC or opaque ETC1S files.	
	uint32_t m_alpha_file_ofs;
	uint32_t m_alpha_file_len;
};

struct basis_file
{
	int m_magic = 0;
	basisu_transcoder m_transcoder;
	std::vector<uint8_t> m_file;

	basis_file(const emscripten::val& jsBuffer)
		: m_file([&]() {
		size_t byteLength = jsBuffer["byteLength"].as<size_t>();
		return std::vector<uint8_t>(byteLength);
			}()),
		m_transcoder(g_pGlobal_codebook)
	{
		if (!g_pGlobal_codebook)
		{
#if BASISU_DEBUG_PRINTF   
			printf("basis_file::basis_file: Must call basis_init() first!\n");
#endif   		
			assert(0);
			return;
		}
	
		unsigned int length = jsBuffer["length"].as<unsigned int>();
		emscripten::val memory = emscripten::val::module_property("HEAP8")["buffer"];
		emscripten::val memoryView = jsBuffer["constructor"].new_(memory, reinterpret_cast<uintptr_t>(m_file.data()), length);
		memoryView.call<void>("set", jsBuffer);

		if (!m_transcoder.validate_header(m_file.data(), m_file.size())) {
			m_file.clear();
		}

		// Initialized after validation
		m_magic = MAGIC;
	}

			void close() {
				assert(m_magic == MAGIC);
				m_file.clear();
			}

			uint32_t getHasAlpha() {
				assert(m_magic == MAGIC);
				if (m_magic != MAGIC)
					return 0;

				basisu_image_level_info li;
				if (!m_transcoder.get_image_level_info(m_file.data(), m_file.size(), li, 0, 0))
					return 0;

				return li.m_alpha_flag;
			}

			uint32_t getNumImages() {
				assert(m_magic == MAGIC);
				if (m_magic != MAGIC)
					return 0;

				return m_transcoder.get_total_images(m_file.data(), m_file.size());
			}

			uint32_t getNumLevels(uint32_t image_index) {
				assert(m_magic == MAGIC);
				if (m_magic != MAGIC)
					return 0;

				basisu_image_info ii;
				if (!m_transcoder.get_image_info(m_file.data(), m_file.size(), ii, image_index))
					return 0;

				return ii.m_total_levels;
			}

			uint32_t getImageWidth(uint32_t image_index, uint32_t level_index) {
				assert(m_magic == MAGIC);
				if (m_magic != MAGIC)
					return 0;

				uint32_t orig_width, orig_height, total_blocks;
				if (!m_transcoder.get_image_level_desc(m_file.data(), m_file.size(), image_index, level_index, orig_width, orig_height, total_blocks))
					return 0;

				return orig_width;
			}

			uint32_t getImageHeight(uint32_t image_index, uint32_t level_index) {
				assert(m_magic == MAGIC);
				if (m_magic != MAGIC)
					return 0;

				uint32_t orig_width, orig_height, total_blocks;
				if (!m_transcoder.get_image_level_desc(m_file.data(), m_file.size(), image_index, level_index, orig_width, orig_height, total_blocks))
					return 0;

				return orig_height;
			}
									
			basis_file_desc getFileDesc() {
				basis_file_desc result;
				memset(&result, 0, sizeof(result));
				
				assert(m_magic == MAGIC);
				if (m_magic != MAGIC)
					return result;
								
				basisu_file_info file_info;
								
				if (!m_transcoder.get_file_info(m_file.data(), m_file.size(), file_info))
				{
					assert(0);
					return result;
				}
				
				result.m_version = file_info.m_version;
				result.m_us_per_frame = file_info.m_us_per_frame;
				result.m_total_images = file_info.m_total_images;
				result.m_userdata0 = file_info.m_userdata0;
				result.m_userdata1 = file_info.m_userdata1;
				result.m_tex_format = static_cast<uint32_t>(file_info.m_tex_format);
				result.m_y_flipped = file_info.m_y_flipped;
				result.m_has_alpha_slices = file_info.m_has_alpha_slices;
				
				result.m_num_endpoints = file_info.m_total_endpoints;
				result.m_endpoint_palette_ofs = file_info.m_endpoint_codebook_ofs;
				result.m_endpoint_palette_len = file_info.m_endpoint_codebook_size;
				
				result.m_num_selectors = file_info.m_total_selectors;
				result.m_selector_palette_ofs = file_info.m_selector_codebook_ofs;
				result.m_selector_palette_len = file_info.m_selector_codebook_size;
				
				result.m_tables_ofs = file_info.m_tables_ofs;
				result.m_tables_len = file_info.m_tables_size;

				return result;
			}
			
			basis_image_desc getImageDesc(uint32_t image_index) {
				basis_image_desc result;
				memset(&result, 0, sizeof(result));
				
				assert(m_magic == MAGIC);
				if (m_magic != MAGIC)
					return result;
								
				basisu_image_info image_info;
				
				// bool get_image_info(const void *pData, uint32_t data_size, basisu_image_info &image_info, uint32_t image_index) const;
				if (!m_transcoder.get_image_info(m_file.data(), m_file.size(), image_info, image_index))
				{
					assert(0);
					return result;
				}
				
				result.m_orig_width = image_info.m_orig_width;
				result.m_orig_height = image_info.m_orig_height;
				result.m_num_blocks_x = image_info.m_num_blocks_x;
				result.m_num_blocks_y = image_info.m_num_blocks_y;
				result.m_num_levels = image_info.m_total_levels;
				result.m_alpha_flag = image_info.m_alpha_flag;
				result.m_iframe_flag = image_info.m_iframe_flag;
				
				return result;
			}
			
			basis_image_level_desc getImageLevelDesc(uint32_t image_index, uint32_t level_index) {
				basis_image_level_desc result;
				memset(&result, 0, sizeof(result));
				
				assert(m_magic == MAGIC);
				if (m_magic != MAGIC)
					return result;
								
				basisu_image_level_info image_info;
				
				if (!m_transcoder.get_image_level_info(m_file.data(), m_file.size(), image_info, image_index, level_index))
				{
					assert(0);
					return result;
				}
				
				result.m_rgb_file_ofs = image_info.m_rgb_file_ofs;
				result.m_rgb_file_len = image_info.m_rgb_file_len;
				result.m_alpha_file_ofs = image_info.m_alpha_file_ofs;
				result.m_alpha_file_len = image_info.m_alpha_file_len;
				
				return result;
			}

			uint32_t getImageTranscodedSizeInBytes(uint32_t image_index, uint32_t level_index, uint32_t format) {
				assert(m_magic == MAGIC);
				if (m_magic != MAGIC)
					return 0;

				if (format >= (int)transcoder_texture_format::cTFTotalTextureFormats)
					return 0;

				uint32_t orig_width, orig_height, total_blocks;
				if (!m_transcoder.get_image_level_desc(m_file.data(), m_file.size(), image_index, level_index, orig_width, orig_height, total_blocks))
					return 0;

				const transcoder_texture_format transcoder_format = static_cast<transcoder_texture_format>(format);

				if (basis_transcoder_format_is_uncompressed(transcoder_format))
				{
					// Uncompressed formats are just plain raster images.
					const uint32_t bytes_per_pixel = basis_get_uncompressed_bytes_per_pixel(transcoder_format);
					const uint32_t bytes_per_line = orig_width * bytes_per_pixel;
					const uint32_t bytes_per_slice = bytes_per_line * orig_height;
					return bytes_per_slice;
				}
				else
				{
					// Compressed formats are 2D arrays of blocks.
					const uint32_t bytes_per_block = basis_get_bytes_per_block_or_pixel(transcoder_format);

					if (transcoder_format == transcoder_texture_format::cTFPVRTC1_4_RGB || transcoder_format == transcoder_texture_format::cTFPVRTC1_4_RGBA)
					{
						// For PVRTC1, Basis only writes (or requires) total_blocks * bytes_per_block. But GL requires extra padding for very small textures: 
						 // https://www.khronos.org/registry/OpenGL/extensions/IMG/IMG_texture_compression_pvrtc.txt
						const uint32_t width = (orig_width + 3) & ~3;
						const uint32_t height = (orig_height + 3) & ~3;
						const uint32_t size_in_bytes = (std::max(8U, width) * std::max(8U, height) * 4 + 7) / 8;
						return size_in_bytes;
					}

					return total_blocks * bytes_per_block;
				}
			}

			bool isUASTC() {
				assert(m_magic == MAGIC);
				if (m_magic != MAGIC)
					return false;

				return m_transcoder.get_tex_format(m_file.data(), m_file.size()) == basis_tex_format::cUASTC4x4;
			}

			uint32_t startTranscoding() {
				assert(m_magic == MAGIC);
				if (m_magic != MAGIC)
					return 0;

				return m_transcoder.start_transcoding(m_file.data(), m_file.size());
			}

			uint32_t transcodeImage(const emscripten::val& dst, uint32_t image_index, uint32_t level_index, uint32_t format, uint32_t unused, uint32_t get_alpha_for_opaque_formats) {
				(void)unused;

				assert(m_magic == MAGIC);
				if (m_magic != MAGIC)
					return 0;

				if (format >= (int)transcoder_texture_format::cTFTotalTextureFormats)
					return 0;

				const transcoder_texture_format transcoder_format = static_cast<transcoder_texture_format>(format);

				uint32_t orig_width, orig_height, total_blocks;
				if (!m_transcoder.get_image_level_desc(m_file.data(), m_file.size(), image_index, level_index, orig_width, orig_height, total_blocks))
					return 0;

				std::vector<uint8_t> dst_data;

				uint32_t flags = get_alpha_for_opaque_formats ? cDecodeFlagsTranscodeAlphaDataToOpaqueFormats : 0;

				uint32_t status;

				if (basis_transcoder_format_is_uncompressed(transcoder_format))
				{
					const uint32_t bytes_per_pixel = basis_get_uncompressed_bytes_per_pixel(transcoder_format);
					const uint32_t bytes_per_line = orig_width * bytes_per_pixel;
					const uint32_t bytes_per_slice = bytes_per_line * orig_height;

					dst_data.resize(bytes_per_slice);

					status = m_transcoder.transcode_image_level(
						m_file.data(), m_file.size(), image_index, level_index,
						dst_data.data(), orig_width * orig_height,
						transcoder_format,
						flags,
						orig_width,
						nullptr,
						orig_height);
				}
				else
				{
					uint32_t bytes_per_block = basis_get_bytes_per_block_or_pixel(transcoder_format);

					uint32_t required_size = total_blocks * bytes_per_block;

					if (transcoder_format == transcoder_texture_format::cTFPVRTC1_4_RGB || transcoder_format == transcoder_texture_format::cTFPVRTC1_4_RGBA)
					{
						// For PVRTC1, Basis only writes (or requires) total_blocks * bytes_per_block. But GL requires extra padding for very small textures: 
						// https://www.khronos.org/registry/OpenGL/extensions/IMG/IMG_texture_compression_pvrtc.txt
						// The transcoder will clear the extra bytes followed the used blocks to 0.
						const uint32_t width = (orig_width + 3) & ~3;
						const uint32_t height = (orig_height + 3) & ~3;
						required_size = (std::max(8U, width) * std::max(8U, height) * 4 + 7) / 8;
						assert(required_size >= total_blocks * bytes_per_block);
					}

					dst_data.resize(required_size);

					status = m_transcoder.transcode_image_level(
						m_file.data(), m_file.size(), image_index, level_index,
						dst_data.data(), dst_data.size() / bytes_per_block,
						static_cast<basist::transcoder_texture_format>(format),
						flags);
				}

				emscripten::val memory = emscripten::val::module_property("HEAP8")["buffer"];
				emscripten::val memoryView = emscripten::val::global("Uint8Array").new_(memory, reinterpret_cast<uintptr_t>(dst_data.data()), dst_data.size());

				dst.call<void>("set", memoryView);
				return status;
			}
};

#if BASISU_SUPPORT_ENCODING
class basis_encoder
{
public:
	basis_compressor_params m_params;

	basis_encoder()
	{
	}

	bool set_slice_source_image(uint32_t slice_index, const emscripten::val& src_image_js_val, uint32_t src_image_width, uint32_t src_image_height, bool src_image_is_png)
	{
		// Resize the source_images array if necessary
		if (slice_index >= m_params.m_source_images.size())
			m_params.m_source_images.resize(slice_index + 1);

		// First copy the src image buffer to the heap.
		std::vector<uint8_t> src_image_buf;
		copy_from_jsbuffer(src_image_js_val, src_image_buf);

		// Now extract the source image.
		image& src_img = m_params.m_source_images[slice_index];
		if (src_image_is_png)
		{
			// It's a PNG file, so try and parse it.
			if (!load_png(src_image_buf.data(), src_image_buf.size(), src_img, nullptr))
			{
#if BASISU_DEBUG_PRINTF
				printf("basis_encoder::set_slice_source_image: Failed parsing provided PNG file!\n");
#endif   
				return false;
			}

			src_image_width = src_img.get_width();
			src_image_height = src_img.get_height();
		}
		else
		{
			// It's a raw image, so check the buffer's size.
			if (src_image_buf.size() != src_image_width * src_image_height * sizeof(uint32_t))
			{
#if BASISU_DEBUG_PRINTF
				printf("basis_encoder::set_slice_source_image: Provided source buffer has an invalid size!\n");
#endif   
				return false;
			}

			// Copy the raw image's data into our source image.
			src_img.resize(src_image_width, src_image_height);
			memcpy(src_img.get_ptr(), src_image_buf.data(), src_image_width * src_image_height * sizeof(uint32_t));
		}
		
		return true;
	}
	
	uint32_t encode(const emscripten::val& dst_basis_file_js_val)
	{
		if (!g_pGlobal_codebook)
		{
#if BASISU_DEBUG_PRINTF   
			printf("basis_encoder::encode: Must call basis_init() first!\n");
#endif   		
			assert(0);
			return 0;
		}

		basist::etc1_global_selector_codebook sel_codebook(basist::g_global_selector_cb_size, basist::g_global_selector_cb);

		// We don't use threading for now, but the compressor needs a job pool.
		job_pool jpool(1);

		// Initialize the compression parameters structure. This is the same structure that the command line tool fills in.
		basis_compressor_params &params = m_params;

		params.m_pJob_pool = &jpool;

		// Disabling multithreading for now, which sucks.
		params.m_multithreading = false;
		
		params.m_status_output = params.m_debug;

		params.m_read_source_images = false;
		params.m_write_output_basis_files = false;
		params.m_pSel_codebook = &sel_codebook;

		basis_compressor comp;

		if (!comp.init(params))
		{
#if BASISU_DEBUG_PRINTF   
			printf("Failed initializing BasisU compressor! One or more provided parameters may be invalid.\n");
#endif	  
			return 0;
		}

#if BASISU_DEBUG_PRINTF   
		printf("Begin BasisU compression\n");
#endif   

		basis_compressor::error_code ec = comp.process();

#if BASISU_DEBUG_PRINTF
		printf("BasisU compression done, status %u, %u bytes\n", (uint32_t)ec, (uint32_t)comp.get_output_basis_file().size());
#endif   

		if (ec != basis_compressor::cECSuccess)
		{
			// Something failed during compression.
#if BASISU_DEBUG_PRINTF	  
			printf("BasisU compression failed with status %u!\n", (uint32_t)ec);
#endif	  
			return 0;
		}

		// Compression succeeded, so copy the .basis file bytes to the caller's buffer.
		if (!copy_to_jsbuffer(dst_basis_file_js_val, comp.get_output_basis_file()))
			return 0;

		// Return the file size of the .basis file in bytes.
		return (uint32_t)comp.get_output_basis_file().size();
	}
};
#endif

class lowlevel_etc1s_image_transcoder : public basisu_lowlevel_etc1s_transcoder
{
	// Using our own transcoder state, for video support.
	basisu_transcoder_state m_state;
	
public:
	lowlevel_etc1s_image_transcoder() : 
		basisu_lowlevel_etc1s_transcoder(g_pGlobal_codebook)
	{
	}
	
	bool decode_palettes(uint32_t num_endpoints, const emscripten::val& endpoint_data, uint32_t num_selectors, const emscripten::val& selector_data)
	{
		std::vector<uint8_t> temp_endpoint_data, temp_selector_data;
		copy_from_jsbuffer(endpoint_data, temp_endpoint_data);
		copy_from_jsbuffer(selector_data, temp_selector_data);

#if 0		
		printf("decode_palettes: %u %u %u %u, %u %u\n", 
			num_endpoints, (uint32_t)temp_endpoint_data.size(),
			num_selectors, (uint32_t)temp_selector_data.size(),
			temp_endpoint_data[0], temp_selector_data[0]);
#endif			
		
		if (!temp_endpoint_data.size() || !temp_selector_data.size())
		{
#if BASISU_DEBUG_PRINTF   
			printf("decode_tables: endpoint_data and/or selector_data is empty\n");
#endif   	
			assert(0);
			return false;
		}
		
		return basisu_lowlevel_etc1s_transcoder::decode_palettes(num_endpoints, &temp_endpoint_data[0], (uint32_t)temp_endpoint_data.size(), 
			num_selectors, &temp_selector_data[0], (uint32_t)temp_selector_data.size());
	}
	
	bool decode_tables(const emscripten::val& table_data)
	{
		std::vector<uint8_t> temp_table_data;
		copy_from_jsbuffer(table_data, temp_table_data);
		
		if (!temp_table_data.size())
		{
#if BASISU_DEBUG_PRINTF   
			printf("decode_tables: table_data is empty\n");
#endif   	
			assert(0);
			return false;
		}
		
		return basisu_lowlevel_etc1s_transcoder::decode_tables(&temp_table_data[0], (uint32_t)temp_table_data.size());
	}
			
	bool transcode_image(
		uint32_t target_format, // see transcoder_texture_format
		const emscripten::val& output_blocks, uint32_t output_blocks_buf_size_in_blocks_or_pixels,
		const emscripten::val& compressed_data,
		uint32_t num_blocks_x, uint32_t num_blocks_y, uint32_t orig_width, uint32_t orig_height, uint32_t level_index, 
		uint32_t rgb_offset, uint32_t rgb_length, uint32_t alpha_offset, uint32_t alpha_length,
		uint32_t decode_flags, // see cDecodeFlagsPVRTCDecodeToNextPow2
		bool basis_file_has_alpha_slices,
		bool is_video,
		uint32_t output_row_pitch_in_blocks_or_pixels,
		uint32_t output_rows_in_pixels)
	{
		if (!g_pGlobal_codebook)
		{
#if BASISU_DEBUG_PRINTF   
			printf("transcode_etc1s_image: basis_init() must be called first\n");
#endif   	
			assert(0);
			return false;
		}
		
		// FIXME: Access the JavaScript buffer directly vs. copying it.
		std::vector<uint8_t> temp_comp_data;
		copy_from_jsbuffer(compressed_data, temp_comp_data);
		
		if (!temp_comp_data.size())
		{
#if BASISU_DEBUG_PRINTF   
			printf("transcode_etc1s_image: compressed_data is empty\n");
#endif   
			assert(0);
			return false;
		}
		
		uint32_t output_blocks_len = output_blocks["byteLength"].as<uint32_t>();
		if (!output_blocks_len)
		{
#if BASISU_DEBUG_PRINTF   
			printf("transcode_etc1s_image: output_blocks is empty\n");
#endif   
			assert(0);
			return false;
		}
		
		std::vector<uint8_t> temp_output_blocks(output_blocks_len);
						
		bool status = basisu_lowlevel_etc1s_transcoder::transcode_image(
			(transcoder_texture_format)target_format,
			&temp_output_blocks[0], output_blocks_buf_size_in_blocks_or_pixels,
			&temp_comp_data[0], temp_comp_data.size(),
			num_blocks_x, num_blocks_y, orig_width, orig_height, level_index,
			rgb_offset, rgb_length, alpha_offset, alpha_length,
			decode_flags,
			basis_file_has_alpha_slices,
			is_video,
			output_row_pitch_in_blocks_or_pixels,
			&m_state,
			output_rows_in_pixels);
			
		if (!status)
		{
#if BASISU_DEBUG_PRINTF   
			printf("transcode_etc1s_image: basisu_lowlevel_etc1s_transcoder::transcode_image failed\n");
#endif   
			assert(0);
			return false;
		}
		
		if (!copy_to_jsbuffer(output_blocks, temp_output_blocks))
			return false;

		return true;
	}
};

bool transcode_uastc_image(
	uint32_t target_format_int, // see transcoder_texture_format
	const emscripten::val& output_blocks, uint32_t output_blocks_buf_size_in_blocks_or_pixels,
	const emscripten::val& compressed_data,
	uint32_t num_blocks_x, uint32_t num_blocks_y, uint32_t orig_width, uint32_t orig_height, uint32_t level_index,
	uint32_t slice_offset, uint32_t slice_length, 
	uint32_t decode_flags, // see cDecodeFlagsPVRTCDecodeToNextPow2
	bool has_alpha,
	bool is_video,
	uint32_t output_row_pitch_in_blocks_or_pixels,
	uint32_t output_rows_in_pixels,
	int channel0, int channel1)
{
	transcoder_texture_format target_format = static_cast<transcoder_texture_format>(target_format_int);
	
	if (!g_pGlobal_codebook)
	{
#if BASISU_DEBUG_PRINTF   
		printf("transcode_uastc_image: basis_init() must be called first\n");
#endif   	
		assert(0);
		return false;
	}
			
	// FIXME: Access the JavaScript buffer directly vs. copying it.
	std::vector<uint8_t> temp_comp_data;
	copy_from_jsbuffer(compressed_data, temp_comp_data);
	
	if (!temp_comp_data.size())
	{
#if BASISU_DEBUG_PRINTF   
		printf("transcode_uastc_image: compressed_data is empty\n");
#endif   
		assert(0);
		return false;
	}
	
	uint32_t output_blocks_len = output_blocks["byteLength"].as<uint32_t>();
	if (!output_blocks_len)
	{
#if BASISU_DEBUG_PRINTF   
		printf("transcode_uastc_image: output_blocks is empty\n");
#endif   
		assert(0);
		return false;
	}

#if 0	
	printf("format: %u\n", (uint32_t)target_format);
	printf("output_blocks size: %u buf size: %u\n", output_blocks_len, output_blocks_buf_size_in_blocks_or_pixels);
	printf("compressed_data size: %u\n", compressed_data["byteLength"].as<uint32_t>());
	printf("%u %u %u %u %u\n", num_blocks_x, num_blocks_y, orig_width, orig_height, level_index);
	printf("%u %u\n", slice_offset, slice_length);
	printf("%u\n", decode_flags);
	printf("has_alpha: %u is_video: %u\n", has_alpha, is_video);
#endif	
	
	std::vector<uint8_t> temp_output_blocks(output_blocks_len);
	
	basisu_lowlevel_uastc_transcoder transcoder;
	
	bool status = transcoder.transcode_image(
		(transcoder_texture_format)target_format,
		&temp_output_blocks[0], output_blocks_buf_size_in_blocks_or_pixels,
		&temp_comp_data[0], temp_comp_data.size(),		
		num_blocks_x, num_blocks_y, orig_width, orig_height, level_index,
		slice_offset, slice_length,
		decode_flags,
		has_alpha,
		is_video,
		output_row_pitch_in_blocks_or_pixels,
		nullptr,
		output_rows_in_pixels,
		channel0, channel1);
	
	if (!status)
	{
#if BASISU_DEBUG_PRINTF   
		printf("transcode_uastc_image: basisu_lowlevel_uastc_transcoder::transcode_image failed\n");
#endif   
		assert(0);
		return false;
	}
	
	if (!copy_to_jsbuffer(output_blocks, temp_output_blocks))
		return false;

	return true;
}

uint32_t get_bytes_per_block_or_pixel(uint32_t transcoder_tex_fmt)
{
	return basis_get_bytes_per_block_or_pixel(static_cast<transcoder_texture_format>(transcoder_tex_fmt));
}

bool format_has_alpha(uint32_t transcoder_tex_fmt)
{
	return basis_transcoder_format_has_alpha(static_cast<transcoder_texture_format>(transcoder_tex_fmt));
}

bool format_is_uncompressed(uint32_t transcoder_tex_fmt)
{
	return basis_transcoder_format_is_uncompressed(static_cast<transcoder_texture_format>(transcoder_tex_fmt));
}

bool is_format_supported(uint32_t transcoder_tex_fmt)
{
	return basis_is_format_supported(static_cast<transcoder_texture_format>(transcoder_tex_fmt));
}

uint32_t get_format_block_width(uint32_t transcoder_tex_fmt)
{
	return basis_get_block_width(static_cast<transcoder_texture_format>(transcoder_tex_fmt));
}

uint32_t get_format_block_height(uint32_t transcoder_tex_fmt)
{
	return basis_get_block_height(static_cast<transcoder_texture_format>(transcoder_tex_fmt));
}

EMSCRIPTEN_BINDINGS(basis_codec) {
  function("initializeBasis", &basis_init);

  // Expose BasisFileDesc structure
  value_object<basis_file_desc>("BasisFileDesc")
	  .field("version", &basis_file_desc::m_version)
	  .field("usPerFrame", &basis_file_desc::m_us_per_frame)
	  .field("totalImages", &basis_file_desc::m_total_images)
	  .field("userdata0", &basis_file_desc::m_userdata0)
	  .field("userdata1", &basis_file_desc::m_userdata1)
	  .field("texFormat", &basis_file_desc::m_tex_format)
	  .field("yFlipped", &basis_file_desc::m_y_flipped)
	  .field("hasAlphaSlices", &basis_file_desc::m_has_alpha_slices)
	  .field("numEndpoints", &basis_file_desc::m_num_endpoints)
	  .field("endpointPaletteOfs", &basis_file_desc::m_endpoint_palette_ofs)
	  .field("endpointPaletteLen", &basis_file_desc::m_endpoint_palette_len)
	  .field("numSelectors", &basis_file_desc::m_num_selectors)
	  .field("selectorPaletteOfs", &basis_file_desc::m_selector_palette_ofs)
	  .field("selectorPaletteLen", &basis_file_desc::m_selector_palette_len)
	  .field("tablesOfs", &basis_file_desc::m_tables_ofs)
	  .field("tablesLen", &basis_file_desc::m_tables_len)
    ;

  // Expose BasisImageDesc structure  	
  value_object<basis_image_desc>("BasisImageDesc")
    .field("origWidth", &basis_image_desc::m_orig_width)		
    .field("origHeight", &basis_image_desc::m_orig_height)
    .field("numBlocksX", &basis_image_desc::m_num_blocks_x)
    .field("numBlocksY", &basis_image_desc::m_num_blocks_y)
    .field("numLevels", &basis_image_desc::m_num_levels)
    .field("alphaFlag", &basis_image_desc::m_alpha_flag)
    .field("iframeFlag", &basis_image_desc::m_iframe_flag)
    ;	  

  // Expose BasisImageLevelDesc structure
  value_object<basis_image_level_desc>("BasisImageLevelDesc")
    .field("rgbFileOfs", &basis_image_level_desc::m_rgb_file_ofs)		
    .field("rgbFileLen", &basis_image_level_desc::m_rgb_file_len)
    .field("alphaFileOfs", &basis_image_level_desc::m_alpha_file_ofs)
    .field("alphaFileLen", &basis_image_level_desc::m_alpha_file_len)
    ;	  	

  // Expose some key enums to JavaScript code.

  // enum class transcoder_texture_format
  enum_<transcoder_texture_format>("transcoder_texture_format")
        .value("cTFETC1_RGB", transcoder_texture_format::cTFETC1_RGB)
		.value("cTFETC2_RGBA", transcoder_texture_format::cTFETC2_RGBA)
		.value("cTFBC1_RGB", transcoder_texture_format::cTFBC1_RGB)
		.value("cTFBC3_RGBA", transcoder_texture_format::cTFBC3_RGBA)
		.value("cTFBC4_R", transcoder_texture_format::cTFBC4_R)
		.value("cTFBC5_RG", transcoder_texture_format::cTFBC5_RG)
		.value("cTFBC7_RGBA", transcoder_texture_format::cTFBC7_RGBA)		
		.value("cTFPVRTC1_4_RGB", transcoder_texture_format::cTFPVRTC1_4_RGB)		
		.value("cTFPVRTC1_4_RGBA", transcoder_texture_format::cTFPVRTC1_4_RGBA)		
		.value("cTFASTC_4x4_RGBA", transcoder_texture_format::cTFASTC_4x4_RGBA)		
		.value("cTFATC_RGB", transcoder_texture_format::cTFATC_RGB)		
		.value("cTFATC_RGBA", transcoder_texture_format::cTFATC_RGBA)		
		.value("cTFFXT1_RGB", transcoder_texture_format::cTFFXT1_RGB)		
		.value("cTFPVRTC2_4_RGB", transcoder_texture_format::cTFPVRTC2_4_RGB)		
		.value("cTFPVRTC2_4_RGBA", transcoder_texture_format::cTFPVRTC2_4_RGBA)		
		.value("cTFETC2_EAC_R11", transcoder_texture_format::cTFETC2_EAC_R11)		
		.value("cTFETC2_EAC_RG11", transcoder_texture_format::cTFETC2_EAC_RG11)		
		.value("cTFRGBA32", transcoder_texture_format::cTFRGBA32)		
		.value("cTFRGB565", transcoder_texture_format::cTFRGB565)		
		.value("cTFBGR565", transcoder_texture_format::cTFBGR565)		
		.value("cTFRGBA4444", transcoder_texture_format::cTFRGBA4444)		
		.value("cTFTotalTextureFormats", transcoder_texture_format::cTFTotalTextureFormats)		
	;

	// Expose some useful transcoder_texture_format helper functions
	function("getBytesPerBlockOrPixel", &get_bytes_per_block_or_pixel);
	function("formatHasAlpha", &format_has_alpha);
	function("formatIsUncompressed", &format_is_uncompressed);
	function("isFormatSupported", &is_format_supported);
	function("getFormatBlockWidth", &get_format_block_width);
	function("getFormatBlockHeight", &get_format_block_height);

	// Expose enum basis_texture_type
	enum_<basis_texture_type>("basis_texture_type")
   		.value("cBASISTexType2D", cBASISTexType2D)	
		.value("cBASISTexType2DArray", cBASISTexType2DArray)	
		.value("cBASISTexTypeCubemapArray", cBASISTexTypeCubemapArray)	
		.value("cBASISTexTypeVideoFrames", cBASISTexTypeVideoFrames)	
		.value("cBASISTexTypeVolume", cBASISTexTypeVolume)	
	;
	
	// Expose enum basis_tex_format
	enum_<basis_tex_format>("basis_tex_format")
   		.value("cETC1S", basis_tex_format::cETC1S)	
		.value("cUASTC4x4", basis_tex_format::cUASTC4x4)	
	;
   
  // Transcoder object. If all you want to do is transcode already encoded .basis files, this is all you really need.
  class_<basis_file>("BasisFile")
    .constructor<const emscripten::val&>()
    .function("close", optional_override([](basis_file& self) {
      return self.close();
    }))
    .function("getHasAlpha", optional_override([](basis_file& self) {
      return self.getHasAlpha();
    }))
    .function("isUASTC", optional_override([](basis_file& self) {
      return self.isUASTC();
    }))
    .function("getNumImages", optional_override([](basis_file& self) {
      return self.getNumImages();
    }))
    .function("getNumLevels", optional_override([](basis_file& self, uint32_t imageIndex) {
      return self.getNumLevels(imageIndex);
    }))
    .function("getImageWidth", optional_override([](basis_file& self, uint32_t imageIndex, uint32_t levelIndex) {
      return self.getImageWidth(imageIndex, levelIndex);
    }))
    .function("getImageHeight", optional_override([](basis_file& self, uint32_t imageIndex, uint32_t levelIndex) {
      return self.getImageHeight(imageIndex, levelIndex);
    }))
	// format is enum class transcoder_texture_format
    .function("getImageTranscodedSizeInBytes", optional_override([](basis_file& self, uint32_t imageIndex, uint32_t levelIndex, uint32_t format) {
      return self.getImageTranscodedSizeInBytes(imageIndex, levelIndex, format);
    }))
    .function("startTranscoding", optional_override([](basis_file& self) {
      return self.startTranscoding();
    }))
	// format is enum class transcoder_texture_format
    .function("transcodeImage", optional_override([](basis_file& self, const emscripten::val& dst, uint32_t imageIndex, uint32_t levelIndex, uint32_t format, uint32_t unused, uint32_t getAlphaForOpaqueFormats) {
      return self.transcodeImage(dst, imageIndex, levelIndex, format, unused, getAlphaForOpaqueFormats);
    }))
	// Returns low-level information about the basis file.
	.function("getFileDesc", optional_override([](basis_file& self) {
      return self.getFileDesc();
    }))
	// Returns low-level information about a specific image in a basis file. An image can contain 1 or more mipmap levels.
	.function("getImageDesc", optional_override([](basis_file& self, uint32_t imageIndex) {
      return self.getImageDesc(imageIndex);
    }))
	// Returns low-level information about a specific image mipmap level in a basis file.
	.function("getImageLevelDesc", optional_override([](basis_file& self, uint32_t imageIndex, uint32_t levelIndex) {
      return self.getImageLevelDesc(imageIndex, levelIndex);
    }))

  ;
      
  // Low-level container independent transcoding of ETC1S and UASTC slice data.
  // These functions allow the caller to transcode compressed ETC1S or UASTC texture data that is embedded within arbitrary data files, such as from KTX2.
  enum_<basisu_decode_flags>("basisu_decode_flags")
	.value("cDecodeFlagsPVRTCDecodeToNextPow2", cDecodeFlagsPVRTCDecodeToNextPow2)	
	.value("cDecodeFlagsTranscodeAlphaDataToOpaqueFormats", cDecodeFlagsTranscodeAlphaDataToOpaqueFormats)	
	.value("cDecodeFlagsBC1ForbidThreeColorBlocks", cDecodeFlagsBC1ForbidThreeColorBlocks)	
	.value("cDecodeFlagsOutputHasAlphaIndices", cDecodeFlagsOutputHasAlphaIndices)	
	.value("cDecodeFlagsHighQuality", cDecodeFlagsHighQuality)	
  ;
  
  // The low-level ETC1S transcoder is a class because it has persistent state (such as the endpoint/selector codebooks and Huffman tables, and transcoder state for video)
  class_<lowlevel_etc1s_image_transcoder>("LowLevelETC1SImageTranscoder")
  	.constructor<>()
	.function("decodePalettes", &lowlevel_etc1s_image_transcoder::decode_palettes)
	.function("decodeTables", &lowlevel_etc1s_image_transcoder::decode_tables)
	.function("transcodeImage", &lowlevel_etc1s_image_transcoder::transcode_image)
   ;  

  // The low-level UASTC transcoder is a single function.   
  function("transcodeUASTCImage", &transcode_uastc_image);

  // Optional encoding/compression support
  
#if BASISU_SUPPORT_ENCODING

	// Compressor Constants
	
    constant("BASISU_MAX_SUPPORTED_TEXTURE_DIMENSION", BASISU_MAX_SUPPORTED_TEXTURE_DIMENSION);
	constant("BASISU_DEFAULT_ENDPOINT_RDO_THRESH", BASISU_DEFAULT_ENDPOINT_RDO_THRESH);
	constant("BASISU_DEFAULT_SELECTOR_RDO_THRESH", BASISU_DEFAULT_SELECTOR_RDO_THRESH);
	constant("BASISU_DEFAULT_QUALITY", BASISU_DEFAULT_QUALITY);
	constant("BASISU_DEFAULT_HYBRID_SEL_CB_QUALITY_THRESH", BASISU_DEFAULT_HYBRID_SEL_CB_QUALITY_THRESH);
	constant("BASISU_MAX_IMAGE_DIMENSION", BASISU_MAX_IMAGE_DIMENSION);
	constant("BASISU_QUALITY_MIN", BASISU_QUALITY_MIN);
	constant("BASISU_QUALITY_MAX", BASISU_QUALITY_MAX);
	constant("BASISU_MAX_ENDPOINT_CLUSTERS", BASISU_MAX_ENDPOINT_CLUSTERS);
	constant("BASISU_MAX_SELECTOR_CLUSTERS", BASISU_MAX_SELECTOR_CLUSTERS);
	constant("BASISU_MAX_SLICES", BASISU_MAX_SLICES);
	constant("BASISU_RDO_UASTC_DICT_SIZE_DEFAULT", BASISU_RDO_UASTC_DICT_SIZE_DEFAULT);
	constant("BASISU_RDO_UASTC_DICT_SIZE_MIN", BASISU_RDO_UASTC_DICT_SIZE_MIN);
	constant("BASISU_RDO_UASTC_DICT_SIZE_MAX", BASISU_RDO_UASTC_DICT_SIZE_MAX);
	constant("BASISU_MAX_RESAMPLER_FILTERS", g_num_resample_filters);
	constant("BASISU_DEFAULT_COMPRESSION_LEVEL", BASISU_DEFAULT_COMPRESSION_LEVEL);
	constant("BASISU_MAX_COMPRESSION_LEVEL", BASISU_MAX_COMPRESSION_LEVEL);
		
	constant("cPackUASTCLevelFastest", cPackUASTCLevelFastest);
	constant("cPackUASTCLevelFaster", cPackUASTCLevelFaster);
	constant("cPackUASTCLevelDefault", cPackUASTCLevelDefault);
	constant("cPackUASTCLevelSlower", cPackUASTCLevelSlower);
	constant("cPackUASTCLevelVerySlow", cPackUASTCLevelVerySlow);
	constant("cPackUASTCLevelMask", cPackUASTCLevelMask);
	constant("cPackUASTCFavorUASTCError", cPackUASTCFavorUASTCError);
	constant("cPackUASTCFavorBC7Error", cPackUASTCFavorBC7Error);
	constant("cPackUASTCETC1FasterHints", cPackUASTCETC1FasterHints);
	constant("cPackUASTCETC1FastestHints", cPackUASTCETC1FastestHints);
	constant("cPackUASTCETC1DisableFlipAndIndividual", cPackUASTCETC1DisableFlipAndIndividual);
	
	constant("UASTC_RDO_DEFAULT_MAX_ALLOWED_RMS_INCREASE_RATIO", UASTC_RDO_DEFAULT_MAX_ALLOWED_RMS_INCREASE_RATIO);
	constant("UASTC_RDO_DEFAULT_SKIP_BLOCK_RMS_THRESH", UASTC_RDO_DEFAULT_SKIP_BLOCK_RMS_THRESH);

  // Compression/encoding object.
  // You create this object, call the set() methods to fill in the parameters/source images/options, call encode(), and you get back a .basis file.
  // You can call .encode() multiple times, changing the parameters/options in between calls.
  class_<basis_encoder>("BasisEncoder")
  	.constructor<>()

	// Compresses the provided source slice(s) to an output .basis file.	
	// At the minimum, you must provided at least 1 source slice by calling setSliceSourceImage() before calling this method.
	.function("encode", optional_override([](basis_encoder& self, const emscripten::val& dst_basis_file_js_val) {
		return self.encode(dst_basis_file_js_val);
	}))

	// Sets the slice's source image, either from a PNG file or from a raw 32-bit RGBA raster image. 
	// If the input is a raster image, the buffer must be width*height*4 bytes in size. The raster image is stored in top down scanline order.
	// The first texel is the top-left texel. The texel byte order in memory is R,G,B,A (R first at offset 0, A last at offset 3).
	// slice_index is the slice to change. Valid range is [0,BASISU_MAX_SLICES-1].
	.function("setSliceSourceImage", optional_override([](basis_encoder& self, uint32_t slice_index, const emscripten::val& src_image_js_val, uint32_t width, uint32_t height, bool src_image_is_png) {
		return self.set_slice_source_image(slice_index, src_image_js_val, width, height, src_image_is_png);
	}))

	// If true, the encoder will output a UASTC texture, otherwise a ETC1S texture.
	.function("setUASTC", optional_override([](basis_encoder& self, bool uastc_flag) {
		self.m_params.m_uastc = uastc_flag;
	}))

	// If true the source images will be Y flipped before compression.	
	.function("setYFlip", optional_override([](basis_encoder& self, bool y_flip_flag) {
		self.m_params.m_y_flip = y_flip_flag;
	}))

	// Enables debug output	to stdout
	.function("setDebug", optional_override([](basis_encoder& self, bool debug_flag) {
		self.m_params.m_debug = debug_flag;
	}))
	
	// If true, the input is assumed to be in sRGB space. Be sure to set this correctly! (Examples: True on photos, albedo/spec maps, and false on normal maps.)
	.function("setPerceptual", optional_override([](basis_encoder& self, bool perceptual_flag) {
		self.m_params.m_perceptual = perceptual_flag;
	}))
	
	// Check source images for active/used alpha channels
	.function("setCheckForAlpha", optional_override([](basis_encoder& self, bool check_for_alpha_flag) {
		self.m_params.m_check_for_alpha = check_for_alpha_flag;
	}))
	
	// Fource output .basis file to have an alpha channel
	.function("setForceAlpha", optional_override([](basis_encoder& self, bool force_alpha_flag) {
		self.m_params.m_force_alpha = force_alpha_flag;
	}))
	
	// Set source image component swizzle.
	// r,g,b,a - valid range is [0,3]
	.function("setSwizzle", optional_override([](basis_encoder& self, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
		assert((r < 4) && (g < 4) && (b < 4) && (a < 4));
		self.m_params.m_swizzle[0] = (char)r;
		self.m_params.m_swizzle[1] = (char)g;
		self.m_params.m_swizzle[2] = (char)b;
		self.m_params.m_swizzle[3] = (char)a;
	}))
	
	// If true, the input is assumed to be a normal map, and all source texels will be renormalized before encoding.
	.function("setRenormalize", optional_override([](basis_encoder& self, bool renormalize_flag) {
		self.m_params.m_renormalize = renormalize_flag;
	}))
	
	// Sets the max # of endpoint clusters for ETC1S mode. Use instead of setQualityLevel.
	// Default is 512, range is [1,BASISU_MAX_ENDPOINT_CLUSTERS]
	.function("setMaxEndpointClusters", optional_override([](basis_encoder& self, uint32_t max_endpoint_clusters) {
		assert(max_endpoint_clusters <= BASISU_MAX_ENDPOINT_CLUSTERS);
		self.m_params.m_max_endpoint_clusters = max_endpoint_clusters;
	}))
	
	// Sets the max # of selectors clusters for ETC1S mode. Use instead of setQualityLevel.
	// Default is 512, range is [1,BASISU_MAX_ENDPOINT_CLUSTERS]
	.function("setMaxSelectorClusters", optional_override([](basis_encoder& self, uint32_t max_selector_clusters) {
		assert(max_selector_clusters <= BASISU_MAX_SELECTOR_CLUSTERS);
		self.m_params.m_max_selector_clusters = max_selector_clusters;
	}))
	
	// Sets the ETC1S encoder's quality level, which controls the file size vs. quality tradeoff.
	// Default is -1 (meaning unused - the compressor will use m_max_endpoint_clusters/m_max_selector_clusters instead to control the codebook sizes).
	// Range is [1,BASISU_QUALITY_MAX]
	.function("setQualityLevel", optional_override([](basis_encoder& self, int quality_level) {
		assert(quality_level >= -1 && quality_level <= BASISU_QUALITY_MAX);
		self.m_params.m_quality_level = quality_level;
	}))

	// The compression_level parameter controls the encoder perf vs. file size tradeoff for ETC1S files. 
	// It does not directly control file size vs. quality - see quality_level().
	// Default is BASISU_DEFAULT_COMPRESSION_LEVEL, range is [0,BASISU_MAX_COMPRESSION_LEVEL]
	.function("setCompressionLevel", optional_override([](basis_encoder& self, int comp_level) {
		assert(comp_level >= 0 && comp_level <= BASISU_MAX_COMPRESSION_LEVEL);
		self.m_params.m_compression_level = comp_level;
	}))
	
	// setNormalMapMode is the same as the basisu.exe "-normal_map" option. It tunes several codec parameters so compression works better on normal maps.
	.function("setNormalMap", optional_override([](basis_encoder& self) {
		self.m_params.m_perceptual = false;
		self.m_params.m_mip_srgb = false;
		self.m_params.m_no_selector_rdo = true;
		self.m_params.m_no_endpoint_rdo = true;
	}))
	
	// Sets selector RDO threshold
	// Default is BASISU_DEFAULT_SELECTOR_RDO_THRESH, range is [0,1e+10]
	.function("setSelectorRDOThresh", optional_override([](basis_encoder& self, float selector_rdo_thresh) {
		self.m_params.m_selector_rdo_thresh = selector_rdo_thresh;
	}))
	
	// Sets endpoint RDO threshold
	// Default is BASISU_DEFAULT_ENDPOINT_RDO_THRESH, range is [0,1e+10]
	.function("setEndpointRDOThresh", optional_override([](basis_encoder& self, float endpoint_rdo_thresh) {
		self.m_params.m_endpoint_rdo_thresh = endpoint_rdo_thresh;
	}))
	
	// --- Mip-map options
	
	// If true mipmaps will be generated from the source images
	.function("setMipGen", optional_override([](basis_encoder& self, bool mip_gen_flag) {
		self.m_params.m_mip_gen = mip_gen_flag;
	}))
	
	// Set mipmap filter's scale factor
	// default is 1, range is [.000125, 4.0]
	.function("setMipScale", optional_override([](basis_encoder& self, float mip_scale) {
		self.m_params.m_mip_scale = mip_scale;
	}))
	
	// Sets the mipmap filter to apply
	// mip_filter must be < BASISU_MAX_RESAMPLER_FILTERS
	// See the end of basisu_resample_filters.cpp: g_resample_filters[]
	.function("setMipFilter", optional_override([](basis_encoder& self, uint32_t mip_filter) {
		assert(mip_filter < g_num_resample_filters);
		if (mip_filter < g_num_resample_filters)
			self.m_params.m_mip_filter = g_resample_filters[mip_filter].name;
	}))
	
	// If true mipmap filtering occurs in sRGB space - this generally should match the perceptual parameter.
	.function("setMipSRGB", optional_override([](basis_encoder& self, bool mip_srgb_flag) {
		self.m_params.m_mip_srgb = mip_srgb_flag;
	}))
	
	// If true, the input is assumed to be a normal map, and the texels of each mipmap will be renormalized before encoding.
	.function("setMipRenormalize", optional_override([](basis_encoder& self, bool mip_renormalize_flag) {
		self.m_params.m_mip_renormalize = mip_renormalize_flag;
	}))
	
	// If true the source texture will be sampled using wrap addressing during mipmap generation, otherwise clamp.
	.function("setMipWrapping", optional_override([](basis_encoder& self, bool mip_wrapping_flag) {
		self.m_params.m_mip_wrapping = mip_wrapping_flag;
	}))

	// Sets the mipmap generator's smallest allowed dimension.	
	// default is 1, range is [1,16384]
	.function("setMipSmallestDimension", optional_override([](basis_encoder& self, int mip_smallest_dimension) {
		self.m_params.m_mip_smallest_dimension = mip_smallest_dimension;
	}))

	// Sets the .basis texture type. 
	// cBASISTexTypeVideoFrames changes the encoder into video mode.
	// tex_type is enum basis_texture_type
	// default is cBASISTexType2D
	.function("setTexType", optional_override([](basis_encoder& self, uint32_t tex_type) {
		assert(tex_type < cBASISTexTypeTotal);
		self.m_params.m_tex_type = (basist::basis_texture_type)tex_type;
	}))
	
	.function("setUserData0", optional_override([](basis_encoder& self, uint32_t userdata0) {
		self.m_params.m_userdata0 = userdata0;
	}))
	
	.function("setUserData1", optional_override([](basis_encoder& self, uint32_t userdata1) {
		self.m_params.m_userdata1 = userdata1;
	}))

	// UASTC specific flags.

	// Sets the UASTC encoding performance vs. quality tradeoff, and other lesser used UASTC encoder flags.
	// This is a combination of flags. See cPackUASTCLevelDefault, etc.
	.function("setPackUASTCFlags", optional_override([](basis_encoder& self, uint32_t pack_uastc_flags) {
		assert((pack_uastc_flags & cPackUASTCLevelMask) >= cPackUASTCLevelFastest);
		assert((pack_uastc_flags & cPackUASTCLevelMask) <= cPackUASTCLevelVerySlow);
		self.m_params.m_pack_uastc_flags = pack_uastc_flags;
	}))

	// If true, the RDO post-processor will be applied to the encoded UASTC texture data.
	.function("setRDOUASTC", optional_override([](basis_encoder& self, bool rdo_uastc) {
		self.m_params.m_rdo_uastc = rdo_uastc;
	}))
	
	// Default is 1.0 range is [0.001, 10.0]
	.function("setRDOUASTCQualityScalar", optional_override([](basis_encoder& self, float rdo_quality) {
		self.m_params.m_rdo_uastc_quality_scalar = rdo_quality;
	}))

    // Default is BASISU_RDO_UASTC_DICT_SIZE_DEFAULT, range is [BASISU_RDO_UASTC_DICT_SIZE_MIN, BASISU_RDO_UASTC_DICT_SIZE_MAX]
	.function("setRDOUASTCDictSize", optional_override([](basis_encoder& self, int dict_size) {
		assert((dict_size >= BASISU_RDO_UASTC_DICT_SIZE_MIN) && (dict_size <= BASISU_RDO_UASTC_DICT_SIZE_MAX));
		self.m_params.m_rdo_uastc_dict_size = dict_size;
	}))

    // Default is UASTC_RDO_DEFAULT_MAX_ALLOWED_RMS_INCREASE_RATIO, range is [01, 100.0]
	.function("setRDOUASTCMaxAllowedRMSIncreaseRatio", optional_override([](basis_encoder& self, float rdo_uastc_max_allowed_rms_increase_ratio) {
		self.m_params.m_rdo_uastc_max_allowed_rms_increase_ratio = rdo_uastc_max_allowed_rms_increase_ratio;
	}))

	// Default is UASTC_RDO_DEFAULT_SKIP_BLOCK_RMS_THRESH, range is [.01f, 100.0f]
	.function("setRDOUASTCSkipBlockRMSThresh", optional_override([](basis_encoder& self, float rdo_uastc_skip_block_rms_thresh) {
		self.m_params.m_rdo_uastc_skip_block_rms_thresh = rdo_uastc_skip_block_rms_thresh;
	}))
	
	// --- Low level options
	
	// Disables selector RDO	
	.function("setNoSelectorRDO", optional_override([](basis_encoder& self, bool no_selector_rdo_flag) {
		self.m_params.m_no_selector_rdo = no_selector_rdo_flag;
	}))
	
	// Disables endpoint RDO
	.function("setNoEndpointRDO", optional_override([](basis_encoder& self, bool no_endpoint_rdo_flag) {
		self.m_params.m_no_endpoint_rdo = no_endpoint_rdo_flag;
	}))
	
	// Display output PSNR statistics
	.function("setComputeStats", optional_override([](basis_encoder& self, bool compute_stats_flag) {
		self.m_params.m_compute_stats = compute_stats_flag;
	}))

	// Write output .PNG files for debugging
	.function("setDebugImages", optional_override([](basis_encoder& self, bool debug_images_flag) {
		self.m_params.m_debug_images = debug_images_flag;
	}))

  ;
#endif // BASISU_SUPPORT_ENCODING
    
}
