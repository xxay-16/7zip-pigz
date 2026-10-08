/* PigzEncoder.cpp -- parallel raw Deflate adapted for 7-Zip streams.
 * The block compression and byte-alignment algorithm is adapted from
 * pigz 2.8 (compress_thread and deflate_engine), Copyright (C) 2007-2023
 * Mark Adler. This is an altered implementation, not the original pigz.
 * The scheduler uses 7-Zip's native thread and event abstractions.
 */

/*
  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the author be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Mark Adler
  madler@alumni.caltech.edu
 */

#include "StdAfx.h"

#include <zlib.h>

#include "../../Common/ComTry.h"
#include "../../Common/MyBuffer.h"

#ifndef Z7_ST
#include "../../Windows/Synchronization.h"
#include "../../Windows/Thread.h"
#endif

#include "../Common/StreamUtils.h"
#include "PigzEncoder.h"

#if ZLIB_VERNUM < 0x1260
#error pigz integration requires zlib 1.2.6 or later
#endif

namespace NCompress {
namespace NPigz {

static const size_t kDictionarySize = 32 << 10;
static const unsigned kMaxThreads = 256;
static const size_t kMaxBlockSize = 16 << 20;

static HRESULT ZlibResult(int result)
{
  return result == Z_OK ? S_OK :
      result == Z_MEM_ERROR ? E_OUTOFMEMORY : E_FAIL;
}

class CWorker
{
  z_stream _stream;
  bool _initialized;

#ifndef Z7_ST
  NWindows::CThread _thread;
  NWindows::NSynchronization::CAutoResetEvent _start;
  NWindows::NSynchronization::CAutoResetEvent _done;
  bool _stop;
  bool _busy;
  HRESULT _result;

  static THREAD_FUNC_DECL ThreadFunc(void *param)
  {
    CWorker &w = *(CWorker *)param;
    for (;;)
    {
      if (w._start.Lock() != 0 || w._stop)
        break;
      w._result = w.Encode();
      if (w._done.Set() != 0)
        break;
    }
    return THREAD_FUNC_RET_ZERO;
  }
#endif

  HRESULT Deflate(int flush)
  {
    do
    {
      const size_t room = Output.Size() - OutputSize;
      if (room == 0)
        return E_FAIL;
      _stream.next_out = Output.NonConstData() + OutputSize;
      _stream.avail_out = (uInt)room;
      const int res = deflate(&_stream, flush);
      OutputSize = (size_t)(_stream.next_out - Output.NonConstData());
      // A repeated Z_BLOCK with no pending input/bits may return Z_BUF_ERROR.
      if (res != Z_OK && !(res == Z_BUF_ERROR && _stream.avail_in == 0))
        return ZlibResult(res);
    }
    while (_stream.avail_out == 0);
    return _stream.avail_in == 0 ? S_OK : E_FAIL;
  }

public:
  CByteBuffer Input;
  CByteBuffer Dictionary;
  CByteBuffer Output;
  size_t InputSize;
  size_t DictionarySize;
  size_t OutputSize;

  CWorker(): _initialized(false),
#ifndef Z7_ST
      _stop(false), _busy(false), _result(S_OK),
#endif
      InputSize(0), DictionarySize(0), OutputSize(0)
  {
    memset(&_stream, 0, sizeof(_stream));
  }

  ~CWorker()
  {
#ifndef Z7_ST
    if (_thread.IsCreated())
    {
      // Join outstanding work before freeing its input or zlib state, also
      // when the caller exits on cancellation, a read error, or a write error.
      if (_busy)
        _done.Lock();
      _stop = true;
      _start.Set();
      _thread.Wait_Close();
    }
#endif
    if (_initialized)
      deflateEnd(&_stream);
  }

  HRESULT Init(unsigned level, size_t blockSize, bool threaded)
  {
    Input.Alloc(blockSize);
    Dictionary.Alloc(kDictionarySize);
    // A conservative bound includes room for the byte-alignment blocks.
    Output.Alloc(blockSize + (blockSize >> 8) + 1024);
    RINOK(ZlibResult(deflateInit2(&_stream, (int)level, Z_DEFLATED, -15,
        8, Z_DEFAULT_STRATEGY)))
    _initialized = true;
#ifndef Z7_ST
    if (threaded)
    {
      WRes res = _start.Create();
      if (res == 0) res = _done.Create();
      if (res == 0) res = _thread.Create(ThreadFunc, this);
      if (res != 0)
        return HRESULT_FROM_WIN32(res);
    }
#else
    (void)threaded;
#endif
    return S_OK;
  }

  HRESULT Encode()
  {
    RINOK(ZlibResult(deflateReset(&_stream)))
    if (DictionarySize != 0)
      RINOK(ZlibResult(deflateSetDictionary(&_stream, Dictionary, (uInt)DictionarySize)))
    _stream.next_in = Input;
    _stream.avail_in = (uInt)InputSize;
    OutputSize = 0;

    // As in pigz, each job emits non-final Deflate blocks ending on a byte
    // boundary. The next job starts with the previous input's 32 KiB history.
    RINOK(Deflate(Z_BLOCK))
    int bits;
    RINOK(ZlibResult(deflatePending(&_stream, NULL, &bits)))
    if (bits & 1)
      RINOK(Deflate(Z_SYNC_FLUSH))
    else if (bits & 7)
    {
      do
      {
        RINOK(ZlibResult(deflatePrime(&_stream, 10, 2)))
        RINOK(ZlibResult(deflatePending(&_stream, NULL, &bits)))
      }
      while (bits & 7);
      RINOK(Deflate(Z_BLOCK))
    }
    return S_OK;
  }

  HRESULT Start()
  {
#ifndef Z7_ST
    if (_thread.IsCreated())
    {
      const WRes res = _start.Set();
      _busy = res == 0;
      return res == 0 ? S_OK : HRESULT_FROM_WIN32(res);
    }
#endif
    return Encode();
  }

  HRESULT Wait()
  {
#ifndef Z7_ST
    if (_busy)
    {
      const WRes res = _done.Lock();
      if (res != 0)
        return HRESULT_FROM_WIN32(res);
      _busy = false;
      return _result;
    }
#endif
    return S_OK;
  }
};

Z7_COM7F_IMF(CEncoder::SetCoderProperties(const PROPID *propIDs,
    const PROPVARIANT *props, UInt32 numProps))
{
  _level = 5;
  _numThreads = 1;
  _blockSize = 128 << 10;
  for (UInt32 i = 0; i < numProps; i++)
  {
    const PROPID id = propIDs[i];
    const PROPVARIANT &prop = props[i];
    if (id == NCoderPropID::kBlockSize || id == NCoderPropID::kBlockSize2)
    {
      UInt64 size;
      if (prop.vt == VT_UI4) size = prop.ulVal;
      else if (prop.vt == VT_UI8) size = prop.uhVal.QuadPart;
      else return E_INVALIDARG;
      if (size < kDictionarySize || size > kMaxBlockSize)
        return E_INVALIDARG;
      _blockSize = (size_t)size;
    }
    else if (id == NCoderPropID::kLevel)
    {
      if (prop.vt != VT_UI4 || prop.ulVal > 9)
        return E_INVALIDARG;
      _level = (unsigned)prop.ulVal;
    }
    else if (id == NCoderPropID::kNumThreads)
    {
      if (prop.vt != VT_UI4 || prop.ulVal == 0)
        return E_INVALIDARG;
      _numThreads = prop.ulVal > kMaxThreads ? kMaxThreads : (unsigned)prop.ulVal;
    }
    else if (id != NCoderPropID::kReduceSize && id != NCoderPropID::kExpectedDataSize)
      return E_INVALIDARG;
  }
#ifdef Z7_ST
  _numThreads = 1;
#endif
  return S_OK;
}

Z7_COM7F_IMF(CEncoder::Code(ISequentialInStream *inStream,
    ISequentialOutStream *outStream, const UInt64 *, const UInt64 *,
    ICompressProgressInfo *progress))
{
  COM_TRY_BEGIN

  CObjArray<CWorker> workers(_numThreads);
  CByteBuffer dictionary(kDictionarySize);
  size_t dictionarySize = 0;
  unsigned readIndex = 0;
  unsigned writeIndex = 0;
  unsigned pending = 0;
  bool eof = false;
  UInt64 inProcessed = 0;
  UInt64 outProcessed = 0;

  // A bounded ring: only the calling thread touches the COM streams and
  // progress callback. Workers never write out of order or retain the file.
  for (;;)
  {
    if (pending == _numThreads || (eof && pending != 0))
    {
      CWorker &w = workers[writeIndex];
      RINOK(w.Wait())
      RINOK(WriteStream(outStream, w.Output, w.OutputSize))
      inProcessed += w.InputSize;
      outProcessed += w.OutputSize;
      pending--;
      if (progress)
        RINOK(progress->SetRatioInfo(&inProcessed, &outProcessed))
      if (++writeIndex == _numThreads)
        writeIndex = 0;
    }

    if (!eof)
    {
      CWorker &w = workers[readIndex];
      if (w.Input.Size() == 0)
        RINOK(w.Init(_level, _blockSize, _numThreads > 1))
      size_t size = _blockSize;
      RINOK(ReadStream(inStream, w.Input, &size))
      if (size == 0)
        eof = true;
      else
      {
        w.InputSize = size;
        w.DictionarySize = dictionarySize;
        if (dictionarySize != 0)
          memcpy(w.Dictionary, dictionary, dictionarySize);
        dictionarySize = size < kDictionarySize ? size : kDictionarySize;
        memcpy(dictionary, w.Input.NonConstData() + size - dictionarySize, dictionarySize);
        RINOK(w.Start())
        pending++;
        if (++readIndex == _numThreads)
          readIndex = 0;
      }
    }

    if (eof && pending == 0)
      break;
  }

  // One final empty fixed-Huffman block closes the single Deflate stream,
  // including for empty input. The gzip handler owns the header, CRC and size.
  static const Byte kFinish[] = { 3, 0 };
  RINOK(WriteStream(outStream, kFinish, sizeof(kFinish)))
  outProcessed += sizeof(kFinish);
  if (progress)
    RINOK(progress->SetRatioInfo(&inProcessed, &outProcessed))
  return S_OK;
  COM_TRY_END
}

}}
