// Copyright 2019-2023 Bloomberg Finance L.P.
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <bmqp_compression.h>

#include <bmqscm_version.h>

#include <bmqu_blob.h>

// BDE
#include <bdlbb_blobutil.h>
#include <bdlma_sequentialallocator.h>
#include <bslma_allocator.h>

// ZLIB
#include <zlib.h>

// ZSTD
#include <zstd.h>

// MemorySanitizer
#if defined(__has_feature)
#if __has_feature(memory_sanitizer)
#include <sanitizer/msan_interface.h>
#endif
#endif
#include <bsl_memory.h>

namespace BloombergLP {
namespace bmqp {

namespace {

extern "C" typedef int (*ZlibStreamMethod)(z_stream*, int);

extern "C" typedef int (*ZlibEndStreamMethod)(z_stream*);

// ===========
// struct ZLib
// ===========

/// This struct provides the utility functions for enabling compression
/// using ZLib algorithm.
struct ZLib {
    // CONSTANTS

    /// The `windowBits` parameter to `::inflateInit2` and `::deflateInit2`
    /// is the base-2 logarithm of the size of the compression window.  15
    /// is generally considered the default, and maximum, size of this
    /// value.  See the documentation in `zlib.h` for more information.
    static const int k_ZLIB_DEFAULT_WINDOW_SIZE = 15;

    /// The memory level parameter to `::deflateInit2` is the base-2
    /// logarithm of the size of the buffer that zlib allocates for internal
    /// data structures.  See the documentation in `zlib.h` for more
    /// information.
    static const int k_ZLIB_DEFAULT_MEM_LEVEL = 8;

    // CLASS METHODS

    /// Return a buffer sufficient to hold the specified number of
    /// `items`, each having the specified `size`, using the specified
    /// `opaque` casted to a `bslma::Allocator *` to supply memory.
    static void*
    zAllocate(void* opaque, unsigned int items, unsigned int size);

    /// Deallocate the buffer at the specified `address` using the specified
    /// `opaque` casted to a `bslma::Allocator *`.
    static void zFree(void* opaque, void* address);

    /// If the specified `stream` is non-zero, output the specified
    /// `baseMessage`, followed by the specified `code`.  Optionally specify
    /// an output `message` to follow the code.  If `message` is 0, no
    /// additional output is generated.
    static void setError(bsl::ostream*            stream,
                         const bslstl::StringRef& baseMessage,
                         int                      code,
                         const char*              message = 0);

    /// Update the `next_in` and `avail_in` values of the specified `stream`
    /// to the next data to read from the specified `input`, if any.  If
    /// `stream` indicates that all current data has been consumed, i.e.,
    /// `0 == stream->avail_in`, increment the specified `index` by one, and
    /// advance the specified `inBuffer` to this index in `input` blob if
    /// the end of `input` has not been reached.  Otherwise, reset `next_in`
    /// to an offset of `avail_in` from the end of `inBuffer` such that only
    /// unread data is consumed.  Return `true` if there is additional input
    /// remaining to be consumed, and `false` otherwise.
    static bool advanceInput(bdlbb::BlobBuffer* inBuffer,
                             z_stream*          stream,
                             int*               index,
                             const bdlbb::Blob& input);

    /// Update the `next_out` and `avail_out` values of the specified
    /// `stream` to an appropriate output location in the specified
    /// `outBuffer`.  If `outBuffer` is full, append it as data to the
    /// specified `output`, and load a new, empty buffer into `outBuffer`
    /// using the specified `factory`. Otherwise, reset `next_out` to an
    /// offset of `avail_out` from the end of `outBuffer` such that
    /// additional data is written into empty space.
    static void advanceOutput(bdlbb::Blob*              output,
                              bdlbb::BlobBuffer*        outBuffer,
                              bdlbb::BlobBufferFactory* factory,
                              z_stream*                 stream);

    /// Apply the operation given by the specified `zlibMethod` and
    /// `zlibEndMethod` on the specified `input` using the specified
    /// `stream`, and write the result to the specified `output`.  If the
    /// specified `maxOutputSize` is non-zero, fail as soon as the
    /// accumulated output would exceed `maxOutputSize` bytes; a value
    /// of 0 means no limit is enforced.  Return 0 on success and
    /// non-zero otherwise, in which case a message is written to the
    /// specified `errorStream` if it is non-zero.
    static int writeOutput(bdlbb::Blob*              output,
                           bdlbb::BlobBufferFactory* factory,
                           z_stream*                 stream,
                           bsl::ostream*             errorStream,
                           const bdlbb::Blob&        input,
                           ZlibStreamMethod          zlibMethod,
                           ZlibEndStreamMethod       zlibEndMethod,
                           bsls::Types::Uint64       maxOutputSize);
};

// ===========
// struct ZLib
// ===========

void* ZLib::zAllocate(void* opaque, unsigned int items, unsigned int size)
{
    bslma::Allocator* allocator = static_cast<bslma::Allocator*>(opaque);
    return allocator->allocate(static_cast<size_t>(items) * size);
}

void ZLib::zFree(void* opaque, void* address)
{
    bslma::Allocator* allocator = static_cast<bslma::Allocator*>(opaque);
    allocator->deallocate(address);
}

void ZLib::setError(bsl::ostream*            stream,
                    const bslstl::StringRef& baseMessage,
                    int                      code,
                    const char*              message)
{
    if (stream) {
        (*stream) << baseMessage << ", Code: " << code;
        if (message) {
            (*stream) << ", Message: " << message;
        }
    }
}

bool ZLib::advanceInput(bdlbb::BlobBuffer* inBuffer,
                        z_stream*          stream,
                        int*               index,
                        const bdlbb::Blob& input)
{
    if (0 == stream->avail_in) {
        // Read the next buffer from the blob.

        if ((1 + *index) == input.numDataBuffers()) {
            // No more input to read.
            return false;  // RETURN
        }

        ++(*index);
        *inBuffer        = input.buffer(*index);
        stream->avail_in = bmqu::BlobUtil::bufferSize(input, *index);
        stream->next_in  = reinterpret_cast<unsigned char*>(inBuffer->data());
    }
    else {
        // Advance the 'next_in' pointer to the next region of unconsumed data
        // in the buffer.

        const ptrdiff_t offset = bmqu::BlobUtil::bufferSize(input, *index) -
                                 stream->avail_in;
        stream->next_in = reinterpret_cast<unsigned char*>(inBuffer->data() +
                                                           offset);
    }

    return true;
}

void ZLib::advanceOutput(bdlbb::Blob*              output,
                         bdlbb::BlobBuffer*        outBuffer,
                         bdlbb::BlobBufferFactory* factory,
                         z_stream*                 stream)
{
    if (0 == stream->avail_out) {
        if (outBuffer->size()) {
// zlib isn't compatible with MemorySanitizer, as per:
//   https://github.com/madler/zlib/issues/518
// To prevent false-positives, we explicitly un-poison the block.
#if defined(__has_feature)
#if __has_feature(memory_sanitizer)
            __msan_unpoison(outBuffer->data(), outBuffer->size());
#endif
#endif

            // Append the previous data buffer to output.
            output->appendDataBuffer(*outBuffer);
        }
        factory->allocate(outBuffer);

        stream->avail_out = outBuffer->size();
        stream->next_out = reinterpret_cast<unsigned char*>(outBuffer->data());
    }
    else {
        // Advance the 'next_out' pointer to the next region of free space in
        // the buffer.

        const ptrdiff_t offset = outBuffer->size() - stream->avail_out;
        stream->next_out = reinterpret_cast<unsigned char*>(outBuffer->data() +
                                                            offset);
    }
}

int ZLib::writeOutput(bdlbb::Blob*              output,
                      bdlbb::BlobBufferFactory* factory,
                      z_stream*                 stream,
                      bsl::ostream*             errorStream,
                      const bdlbb::Blob&        input,
                      ZlibStreamMethod          zlibMethod,
                      ZlibEndStreamMethod       zlibEndMethod,
                      bsls::Types::Uint64       maxOutputSize)
{
    enum RcEnum {
        rc_SUCCESS                = 0,
        rc_STREAM_INIT_FAILURE    = -1,
        rc_STREAM_PROCESS_FAILURE = -2,
        rc_STREAM_END_FAILURE     = -3,
        rc_MAX_SIZE_EXCEEDED      = -4
    };

    bdlbb::BlobBuffer inBuffer;
    bdlbb::BlobBuffer outBuffer;
    int               index = -1;
    int               result;

    // Process input data until all input buffers have been read.
    while (true) {
        if (!advanceInput(&inBuffer, stream, &index, input)) {
            // No more input to read.
            break;  // BREAK
        }
        advanceOutput(output, &outBuffer, factory, stream);

        result = zlibMethod(stream, Z_NO_FLUSH);
        if (Z_OK != result && Z_BUF_ERROR != result &&
            Z_STREAM_END != result) {
            setError(errorStream,
                     "Error processing stream",
                     result,
                     stream->msg);
            zlibEndMethod(stream);
            return rc_STREAM_PROCESS_FAILURE;  // RETURN
        }

        // Fail if the accumulated output would exceed the cap. The current
        // 'outBuffer' has not been appended to 'output' yet, so account
        // for the bytes already written into it.
        if (maxOutputSize != 0 &&
            static_cast<bsls::Types::Uint64>(output->length()) +
                    (outBuffer.size() - stream->avail_out) >
                maxOutputSize) {
            setError(errorStream,
                     "Decompressed output exceeds maximum size",
                     0);
            zlibEndMethod(stream);
            return rc_MAX_SIZE_EXCEEDED;  // RETURN
        }
    }

    // Continue to write output data until the stream reaches its end, or the
    // operation fails.  As an extra sanity check to avoid spinning, we stash
    // the value of 'avail_out' and only continue iterating while bytes are
    // being written to the output.
    unsigned int lastSize;
    do {
        advanceOutput(output, &outBuffer, factory, stream);
        lastSize = stream->avail_out;
        result   = zlibMethod(stream, Z_FINISH);

        // Fail closed if the accumulated output would exceed the cap (see the
        // explanation above).
        if (maxOutputSize != 0 &&
            static_cast<bsls::Types::Uint64>(output->length()) +
                    (outBuffer.size() - stream->avail_out) >
                maxOutputSize) {
            setError(errorStream,
                     "Decompressed output exceeds maximum size",
                     0);
            zlibEndMethod(stream);
            return rc_MAX_SIZE_EXCEEDED;  // RETURN
        }
    } while ((Z_BUF_ERROR == result || Z_OK == result) &&
             lastSize != stream->avail_out);

    zlibEndMethod(stream);

    if (result != Z_STREAM_END) {
        setError(errorStream, "Error finishing stream", result, stream->msg);
        return rc_STREAM_END_FAILURE;  // RETURN
    }

    // If we have written any data to the current output buffer, reduce its
    // size and add it to the blob.
    if (outBuffer.size() && 0 < outBuffer.size() - stream->avail_out) {
        outBuffer.setSize(outBuffer.size() - stream->avail_out);

// zlib isn't compatible with MemorySanitizer, as per:
//   https://github.com/madler/zlib/issues/518
// To prevent false-positives, we explicitly un-poison the block.
#if defined(__has_feature)
#if __has_feature(memory_sanitizer)
        __msan_unpoison(outBuffer.data(), outBuffer.size());
#endif
#endif

        output->appendDataBuffer(outBuffer);
    }

    return rc_SUCCESS;
}

// ===========
// struct Zstd
// ===========

/// This struct provides utility functions for the public Zstandard streaming
/// API used by the compression implementation.
struct Zstd {
    // CLASS METHODS

    /// Prepare `outBuffer` for writing, appending any completed buffer to
    /// `output`.
    static void prepareOutput(bdlbb::Blob*              output,
                              bdlbb::BlobBufferFactory* factory,
                              bdlbb::BlobBuffer*        outBuffer,
                              size_t*                   outSize);

    /// Record a Zstandard error in `stream` when one is supplied.
    static void setError(bsl::ostream*            stream,
                         const bslstl::StringRef& baseMessage,
                         int                      code,
                         const char*              message = 0);
};

void Zstd::prepareOutput(bdlbb::Blob*              output,
                         bdlbb::BlobBufferFactory* factory,
                         bdlbb::BlobBuffer*        outBuffer,
                         size_t*                   outSize)
{
    if (outBuffer->size() == 0 ||
        *outSize == static_cast<size_t>(outBuffer->size())) {
        if (*outSize != 0) {
            outBuffer->setSize(static_cast<int>(*outSize));
            output->appendDataBuffer(*outBuffer);
        }
        factory->allocate(outBuffer);
        *outSize = 0;
    }
}

void Zstd::setError(bsl::ostream*            stream,
                    const bslstl::StringRef& baseMessage,
                    int                      code,
                    const char*              message)
{
    if (stream) {
        (*stream) << baseMessage << ", Code: " << code;
        if (message) {
            (*stream) << ", Message: " << message;
        }
    }
}

/// Wrap the specified character array in a zero-copy blob for streaming
/// compression algorithms.
static void makeInputBlob(bdlbb::Blob*      inputBlob,
                          const char*       input,
                          int               inputLength,
                          bslma::Allocator* allocator)
{
    bsl::shared_ptr<char> inputBufferSp(const_cast<char*>(input),
                                        bslstl::SharedPtrNilDeleter(),
                                        allocator);
    bdlbb::BlobBuffer     inputBlobBuffer(inputBufferSp, inputLength);

    if (inputBlobBuffer.size() > 0) {
        inputBlob->appendDataBuffer(inputBlobBuffer);
    }
}

}  // close unnamed namespace

// ==================
// struct Compression
// ==================

int Compression::compress(bdlbb::Blob*                         output,
                          bdlbb::BlobBufferFactory*            factory,
                          bmqt::CompressionAlgorithmType::Enum algorithm,
                          const bdlbb::Blob&                   input,
                          bsl::ostream*                        errorStream,
                          bslma::Allocator*                    allocator)
{
    // PRECONDITIONS
    BSLS_ASSERT_SAFE(output);
    BSLS_ASSERT_SAFE(factory);

    enum RcEnum { rc_SUCCESS = 0, rc_UNKNOWN_ALGORITHM = -1 };

    switch (algorithm) {
    case bmqt::CompressionAlgorithmType::e_ZLIB:
        return Compression_Impl::compressZlib(output,
                                              factory,
                                              input,
                                              Z_DEFAULT_COMPRESSION,
                                              errorStream,
                                              allocator);  // RETURN
    case bmqt::CompressionAlgorithmType::e_ZSTD:
        return Compression_Impl::compressZstd(output,
                                              factory,
                                              input,
                                              ZSTD_CLEVEL_DEFAULT,
                                              errorStream,
                                              allocator);  // RETURN
    case bmqt::CompressionAlgorithmType::e_NONE:
        if (output->length() == 0) {
            *output = input;
        }
        else {
            bdlbb::BlobUtil::append(output, input);
        }
        return rc_SUCCESS;  // RETURN
    case bmqt::CompressionAlgorithmType::e_UNKNOWN:
    default: return rc_UNKNOWN_ALGORITHM;  // RETURN
    }
}

int Compression::compress(bdlbb::Blob*                         output,
                          bdlbb::BlobBufferFactory*            factory,
                          bmqt::CompressionAlgorithmType::Enum algorithm,
                          const char*                          input,
                          int                                  inputLength,
                          bsl::ostream*                        errorStream,
                          bslma::Allocator*                    allocator)
{
    // PRECONDITIONS
    BSLS_ASSERT_SAFE(output);
    BSLS_ASSERT_SAFE(factory);
    BSLS_ASSERT_SAFE(input);

    enum RcEnum { rc_SUCCESS = 0, rc_UNKNOWN_ALGORITHM = -1 };

    bdlbb::Blob inputBlob(factory, allocator);
    makeInputBlob(&inputBlob, input, inputLength, allocator);
    switch (algorithm) {
    case bmqt::CompressionAlgorithmType::e_ZLIB:
        return Compression_Impl::compressZlib(output,
                                              factory,
                                              inputBlob,
                                              Z_DEFAULT_COMPRESSION,
                                              errorStream,
                                              allocator);  // RETURN
    case bmqt::CompressionAlgorithmType::e_ZSTD:
        return Compression_Impl::compressZstd(output,
                                              factory,
                                              inputBlob,
                                              ZSTD_CLEVEL_DEFAULT,
                                              errorStream,
                                              allocator);  // RETURN
    case bmqt::CompressionAlgorithmType::e_NONE:
        // deep copy of input character array to output Blob
        bdlbb::BlobUtil::append(output, input, inputLength);
        return rc_SUCCESS;  // RETURN
    case bmqt::CompressionAlgorithmType::e_UNKNOWN:
    default: return rc_UNKNOWN_ALGORITHM;  // RETURN
    }
}

int Compression::decompress(bdlbb::Blob*                         output,
                            bdlbb::BlobBufferFactory*            factory,
                            bmqt::CompressionAlgorithmType::Enum algorithm,
                            const bdlbb::Blob&                   input,
                            bsls::Types::Uint64                  maxOutputSize,
                            bsl::ostream*                        errorStream,
                            bslma::Allocator*                    allocator)
{
    enum RcEnum { rc_SUCCESS = 0, rc_UNKNOWN_ALGORITHM = -1 };

    switch (algorithm) {
    case bmqt::CompressionAlgorithmType::e_ZLIB:
        return Compression_Impl::decompressZlib(output,
                                                factory,
                                                input,
                                                maxOutputSize,
                                                errorStream,
                                                allocator);  // RETURN
    case bmqt::CompressionAlgorithmType::e_ZSTD:
        return Compression_Impl::decompressZstd(output,
                                                factory,
                                                input,
                                                maxOutputSize,
                                                errorStream,
                                                allocator);  // RETURN
    case bmqt::CompressionAlgorithmType::e_NONE:
        if (output->length() == 0) {
            *output = input;
        }
        else {
            bdlbb::BlobUtil::append(output, input);
        }
        return rc_SUCCESS;  // RETURN
    case bmqt::CompressionAlgorithmType::e_UNKNOWN:
    default: return rc_UNKNOWN_ALGORITHM;  // RETURN
    }
}

// ======================
// struct CompressionImpl
// ======================

int Compression_Impl::compressZlib(bdlbb::Blob*              output,
                                   bdlbb::BlobBufferFactory* factory,
                                   const bdlbb::Blob&        input,
                                   int                       level,
                                   bsl::ostream*             errorStream,
                                   bslma::Allocator*         allocator)
{
    enum RcEnum { rc_SUCCESS = 0, rc_STREAM_INIT_FAILURE = -1 };

    z_stream stream = {};
    stream.zalloc   = &ZLib::zAllocate;
    stream.zfree    = &ZLib::zFree;
    stream.opaque   = bslma::Default::allocator(allocator);

    const int result = ::deflateInit2(&stream,
                                      level,
                                      Z_DEFLATED,
                                      ZLib::k_ZLIB_DEFAULT_WINDOW_SIZE,
                                      ZLib::k_ZLIB_DEFAULT_MEM_LEVEL,
                                      Z_FILTERED);
    if (Z_OK != result) {
        ZLib::setError(errorStream,
                       "Error initializing deflate stream",
                       result,
                       stream.msg);
        return rc_STREAM_INIT_FAILURE;  // RETURN
    }

    return ZLib::writeOutput(output,
                             factory,
                             &stream,
                             errorStream,
                             input,
                             &::deflate,
                             &::deflateEnd,
                             0);  // no output cap for compression
}

int Compression_Impl::decompressZlib(bdlbb::Blob*              output,
                                     bdlbb::BlobBufferFactory* factory,
                                     const bdlbb::Blob&        input,
                                     bsls::Types::Uint64       maxOutputSize,
                                     bsl::ostream*             errorStream,
                                     bslma::Allocator*         allocator)
{
    enum RcEnum { rc_SUCCESS = 0, rc_STREAM_INIT_FAILURE = -1 };

    z_stream stream = {};
    stream.zalloc   = &ZLib::zAllocate;
    stream.zfree    = &ZLib::zFree;
    stream.opaque   = bslma::Default::allocator(allocator);

    const int result = ::inflateInit2(&stream,
                                      ZLib::k_ZLIB_DEFAULT_WINDOW_SIZE);
    if (Z_OK != result) {
        ZLib::setError(errorStream,
                       "Error initializing inflate stream",
                       result,
                       stream.msg);
        return rc_STREAM_INIT_FAILURE;  // RETURN
    }

    return ZLib::writeOutput(output,
                             factory,
                             &stream,
                             errorStream,
                             input,
                             &::inflate,
                             &::inflateEnd,
                             maxOutputSize);
}

int Compression_Impl::compressZstd(bdlbb::Blob*              output,
                                   bdlbb::BlobBufferFactory* factory,
                                   const bdlbb::Blob&        input,
                                   int                       level,
                                   bsl::ostream*             errorStream,
                                   bslma::Allocator*)
{
    enum RcEnum {
        rc_SUCCESS                = 0,
        rc_STREAM_INIT_FAILURE    = -1,
        rc_STREAM_PROCESS_FAILURE = -2,
        rc_STREAM_END_FAILURE     = -3
    };

    // Use the stable public API and its default allocator for Zstandard
    // contexts; the supplied BDE allocator remains used for blob buffers.
    ZSTD_CStream* stream = ZSTD_createCStream();
    if (!stream) {
        Zstd::setError(errorStream,
                       "Error initializing zstd compression stream",
                       rc_STREAM_INIT_FAILURE);
        return rc_STREAM_INIT_FAILURE;  // RETURN
    }

    size_t result = ZSTD_CCtx_setParameter(stream,
                                           ZSTD_c_compressionLevel,
                                           level);
    if (ZSTD_isError(result)) {
        Zstd::setError(errorStream,
                       "Error configuring zstd compression stream",
                       rc_STREAM_INIT_FAILURE,
                       ZSTD_getErrorName(result));
        ZSTD_freeCStream(stream);
        return rc_STREAM_INIT_FAILURE;  // RETURN
    }

    bdlbb::BlobBuffer outBuffer;
    size_t            outSize = 0;

    for (int index = 0; index < input.numDataBuffers(); ++index) {
        ZSTD_inBuffer in = {
            input.buffer(index).data(),
            static_cast<size_t>(bmqu::BlobUtil::bufferSize(input, index)),
            0};
        while (in.pos < in.size) {
            Zstd::prepareOutput(output, factory, &outBuffer, &outSize);
            ZSTD_outBuffer out = {outBuffer.data(),
                                  static_cast<size_t>(outBuffer.size()),
                                  outSize};
            result  = ZSTD_compressStream2(stream, &out, &in, ZSTD_e_continue);
            outSize = out.pos;
            if (ZSTD_isError(result)) {
                Zstd::setError(errorStream,
                               "Error processing zstd compression stream",
                               rc_STREAM_PROCESS_FAILURE,
                               ZSTD_getErrorName(result));
                ZSTD_freeCStream(stream);
                return rc_STREAM_PROCESS_FAILURE;  // RETURN
            }
        }
    }

    // Emit the frame terminator, including for an empty input blob.
    ZSTD_inBuffer emptyInput = {0, 0, 0};
    do {
        Zstd::prepareOutput(output, factory, &outBuffer, &outSize);
        ZSTD_outBuffer out = {outBuffer.data(),
                              static_cast<size_t>(outBuffer.size()),
                              outSize};
        result  = ZSTD_compressStream2(stream, &out, &emptyInput, ZSTD_e_end);
        outSize = out.pos;
        if (ZSTD_isError(result)) {
            Zstd::setError(errorStream,
                           "Error finishing zstd compression stream",
                           rc_STREAM_END_FAILURE,
                           ZSTD_getErrorName(result));
            ZSTD_freeCStream(stream);
            return rc_STREAM_END_FAILURE;  // RETURN
        }
    } while (result != 0);

    if (outSize != 0) {
        outBuffer.setSize(static_cast<int>(outSize));
        output->appendDataBuffer(outBuffer);
    }

    ZSTD_freeCStream(stream);
    return rc_SUCCESS;
}

int Compression_Impl::decompressZstd(bdlbb::Blob*              output,
                                     bdlbb::BlobBufferFactory* factory,
                                     const bdlbb::Blob&        input,
                                     bsls::Types::Uint64       maxOutputSize,
                                     bsl::ostream*             errorStream,
                                     bslma::Allocator*)
{
    enum RcEnum {
        rc_SUCCESS                = 0,
        rc_STREAM_INIT_FAILURE    = -1,
        rc_STREAM_PROCESS_FAILURE = -2,
        rc_INCOMPLETE_FRAME       = -3,
        rc_MAX_SIZE_EXCEEDED      = -4
    };

    // Use the stable public API and its default allocator for the stream.
    ZSTD_DStream* stream = ZSTD_createDStream();
    if (!stream) {
        Zstd::setError(errorStream,
                       "Error initializing zstd decompression stream",
                       rc_STREAM_INIT_FAILURE);
        return rc_STREAM_INIT_FAILURE;  // RETURN
    }

    size_t result = ZSTD_initDStream(stream);
    if (ZSTD_isError(result)) {
        Zstd::setError(errorStream,
                       "Error initializing zstd decompression stream",
                       rc_STREAM_INIT_FAILURE,
                       ZSTD_getErrorName(result));
        ZSTD_freeDStream(stream);
        return rc_STREAM_INIT_FAILURE;  // RETURN
    }

    bdlbb::BlobBuffer outBuffer;
    size_t            outSize    = 0;
    size_t            lastResult = 1;
    size_t            inputBytes = 0;

    for (int index = 0; index < input.numDataBuffers(); ++index) {
        ZSTD_inBuffer in = {
            input.buffer(index).data(),
            static_cast<size_t>(bmqu::BlobUtil::bufferSize(input, index)),
            0};
        inputBytes += in.size;
        while (in.pos < in.size) {
            Zstd::prepareOutput(output, factory, &outBuffer, &outSize);
            ZSTD_outBuffer out = {outBuffer.data(),
                                  static_cast<size_t>(outBuffer.size()),
                                  outSize};
            lastResult         = ZSTD_decompressStream(stream, &out, &in);
            outSize            = out.pos;
            if (ZSTD_isError(lastResult)) {
                Zstd::setError(errorStream,
                               "Error processing zstd decompression stream",
                               rc_STREAM_PROCESS_FAILURE,
                               ZSTD_getErrorName(lastResult));
                ZSTD_freeDStream(stream);
                return rc_STREAM_PROCESS_FAILURE;  // RETURN
            }

            // Bound output after every streaming call, as in the zlib path.
            if (maxOutputSize != 0 &&
                static_cast<bsls::Types::Uint64>(output->length()) + outSize >
                    maxOutputSize) {
                Zstd::setError(errorStream,
                               "Decompressed output exceeds maximum size",
                               rc_MAX_SIZE_EXCEEDED);
                ZSTD_freeDStream(stream);
                return rc_MAX_SIZE_EXCEEDED;  // RETURN
            }
        }
    }

    // Drain decoded data buffered by Zstandard after all input bytes have
    // been consumed, including when the final input call filled the output
    // buffer before completing the frame.
    if (inputBytes > 0) {
        ZSTD_inBuffer emptyInput = {0, 0, 0};
        while (lastResult != 0) {
            Zstd::prepareOutput(output, factory, &outBuffer, &outSize);
            const size_t   previousResult  = lastResult;
            const size_t   previousOutSize = outSize;
            ZSTD_outBuffer out             = {outBuffer.data(),
                                              static_cast<size_t>(outBuffer.size()),
                                              outSize};
            lastResult = ZSTD_decompressStream(stream, &out, &emptyInput);
            outSize    = out.pos;
            if (ZSTD_isError(lastResult)) {
                Zstd::setError(errorStream,
                               "Error processing zstd decompression stream",
                               rc_STREAM_PROCESS_FAILURE,
                               ZSTD_getErrorName(lastResult));
                ZSTD_freeDStream(stream);
                return rc_STREAM_PROCESS_FAILURE;  // RETURN
            }

            if (maxOutputSize != 0 &&
                static_cast<bsls::Types::Uint64>(output->length()) + outSize >
                    maxOutputSize) {
                Zstd::setError(errorStream,
                               "Decompressed output exceeds maximum size",
                               rc_MAX_SIZE_EXCEEDED);
                ZSTD_freeDStream(stream);
                return rc_MAX_SIZE_EXCEEDED;  // RETURN
            }

            if (outSize == previousOutSize && lastResult == previousResult) {
                break;  // BREAK
            }
        }
    }

    if (inputBytes == 0 || lastResult != 0) {
        Zstd::setError(errorStream,
                       "Error finishing zstd decompression stream",
                       rc_INCOMPLETE_FRAME,
                       "incomplete frame");
        ZSTD_freeDStream(stream);
        return rc_INCOMPLETE_FRAME;  // RETURN
    }

    if (outSize != 0) {
        outBuffer.setSize(static_cast<int>(outSize));
        output->appendDataBuffer(outBuffer);
    }

    ZSTD_freeDStream(stream);
    return rc_SUCCESS;
}

}  // close package namespace
}  // close enterprise namespace
