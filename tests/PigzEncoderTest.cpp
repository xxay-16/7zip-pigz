// Direct stream/error tests for PigzEncoder. Build from the repository root:
// g++ -O2 -Wall -Wextra -Werror -static tests/PigzEncoderTest.cpp
//   CPP/7zip/Bundles/Alone2/_o_pigz/PigzEncoder.o
//   CPP/7zip/Bundles/Alone2/_o_pigz/StreamUtils.o
//   CPP/7zip/Bundles/Alone2/_o_pigz/Threads.o -lz -luuid -o bin/pigz-stream-test.exe

#include "../CPP/Common/Common.h"
#include "../CPP/Common/MyInitGuid.h"
#include "../CPP/7zip/Compress/PigzEncoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <zlib.h>

static const size_t kBlock = 32768;

static void Check(bool condition, const char *what)
{
  if (!condition)
  {
    fprintf(stderr, "FAIL: %s\n", what);
    exit(1);
  }
}

class CInput Z7_final: public ISequentialInStream, public CMyUnknownImp
{
  Z7_IFACES_IMP_UNK_1(ISequentialInStream)
public:
  std::vector<Byte> Data;
  size_t Pos;
  size_t FailAfter;
  UInt32 ReadLimit;
#ifdef _WIN32
  DWORD Owner;
#endif
  CInput(): Pos(0), FailAfter((size_t)-1), ReadLimit(17)
  {
#ifdef _WIN32
    Owner = GetCurrentThreadId();
#endif
    Data.resize(kBlock * 21 + 11);
    UInt32 state = 42;
    for (size_t i = 0; i < Data.size(); i++)
    {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      Data[i] = (Byte)state;
    }
  }
};

Z7_COM7F_IMF(CInput::Read(void *data, UInt32 size, UInt32 *processed))
{
  *processed = 0;
#ifdef _WIN32
  if (Owner != GetCurrentThreadId()) return E_UNEXPECTED;
#endif
  if (Pos >= FailAfter) return E_FAIL;
  if (size > ReadLimit) size = ReadLimit;
  if (size > Data.size() - Pos) size = (UInt32)(Data.size() - Pos);
  if (size > FailAfter - Pos) size = (UInt32)(FailAfter - Pos);
  if (size) memcpy(data, &Data[Pos], size);
  Pos += size;
  *processed = size;
  return S_OK;
}

class COutput Z7_final: public ISequentialOutStream, public CMyUnknownImp
{
  Z7_IFACES_IMP_UNK_1(ISequentialOutStream)
public:
  std::vector<Byte> Data;
  size_t FailAfter;
  UInt32 WriteLimit;
#ifdef _WIN32
  DWORD Owner;
#endif
  COutput(): FailAfter((size_t)-1), WriteLimit(31)
  {
#ifdef _WIN32
    Owner = GetCurrentThreadId();
#endif
  }
};

Z7_COM7F_IMF(COutput::Write(const void *data, UInt32 size, UInt32 *processed))
{
  *processed = 0;
#ifdef _WIN32
  if (Owner != GetCurrentThreadId()) return E_UNEXPECTED;
#endif
  if (Data.size() >= FailAfter) return E_ACCESSDENIED;
  if (size > WriteLimit) size = WriteLimit;
  if (size > FailAfter - Data.size()) size = (UInt32)(FailAfter - Data.size());
  const Byte *bytes = (const Byte *)data;
  Data.insert(Data.end(), bytes, bytes + size);
  *processed = size;
  return S_OK;
}

class CProgress Z7_final: public ICompressProgressInfo, public CMyUnknownImp
{
  Z7_IFACES_IMP_UNK_1(ICompressProgressInfo)
public:
  bool Cancel;
  UInt64 In;
  UInt64 Out;
#ifdef _WIN32
  DWORD Owner;
#endif
  CProgress(): Cancel(false), In(0), Out(0)
  {
#ifdef _WIN32
    Owner = GetCurrentThreadId();
#endif
  }
};

Z7_COM7F_IMF(CProgress::SetRatioInfo(const UInt64 *inSize, const UInt64 *outSize))
{
#ifdef _WIN32
  if (Owner != GetCurrentThreadId()) return E_UNEXPECTED;
#endif
  Check(inSize && outSize && *inSize >= In && *outSize >= Out, "monotonic progress");
  In = *inSize;
  Out = *outSize;
  return Cancel ? E_ABORT : S_OK;
}

static void Verify(const CInput &input, const COutput &output)
{
  std::vector<Byte> plain(input.Data.size() + 1);
  z_stream stream;
  memset(&stream, 0, sizeof(stream));
  Check(inflateInit2(&stream, -15) == Z_OK, "inflateInit2");
  stream.next_in = const_cast<Byte *>(&output.Data[0]);
  stream.avail_in = (uInt)output.Data.size();
  stream.next_out = &plain[0];
  stream.avail_out = (uInt)plain.size();
  Check(inflate(&stream, Z_FINISH) == Z_STREAM_END, "complete raw Deflate stream");
  Check(stream.avail_in == 0 && stream.total_out == input.Data.size(), "raw stream sizes");
  if (!input.Data.empty())
    Check(memcmp(&plain[0], &input.Data[0], input.Data.size()) == 0, "raw stream data");
  inflateEnd(&stream);
}

int main()
{
  NCompress::NPigz::CEncoder implementation;
  ICompressCoder &encoder = implementation;
  ICompressSetCoderProperties &properties = implementation;
  PROPID ids[] = { NCoderPropID::kNumThreads, NCoderPropID::kLevel, NCoderPropID::kBlockSize2 };
  PROPVARIANT props[3];
  memset(props, 0, sizeof(props));
  for (unsigned i = 0; i < 3; i++) props[i].vt = VT_UI4;
  props[0].ulVal = 4;
  props[1].ulVal = 6;
  props[2].ulVal = (ULONG)kBlock;
  Check(properties.SetCoderProperties(ids, props, 3) == S_OK, "coder properties");

  {
    CInput input;
    COutput output;
    CProgress progress;
    const UInt64 misleadingSize = 1;
    Check(encoder.Code(&input, &output, &misleadingSize, NULL, &progress) == S_OK, "short reads/writes");
    Verify(input, output);
    Check(progress.In == input.Data.size() && progress.Out == output.Data.size(), "final progress");
  }

#ifdef _WIN32
  DWORD handlesBefore;
  Check(GetProcessHandleCount(GetCurrentProcess(), &handlesBefore) != 0, "handle count before");
#endif
  for (unsigned repeat = 0; repeat < 12; repeat++)
  {
    for (unsigned fault = 0; fault < 3; fault++)
    {
      CInput input;
      COutput output;
      CProgress progress;
      if (fault == 0) input.FailAfter = 2 * kBlock + 17;
      if (fault == 1) output.FailAfter = 19;
      if (fault == 2) progress.Cancel = true;
      const HRESULT expected = fault == 0 ? E_FAIL : fault == 1 ? E_ACCESSDENIED : E_ABORT;
      Check(encoder.Code(&input, &output, NULL, NULL, &progress) == expected, "propagated stream/cancel error");
    }
  }
#ifdef _WIN32
  DWORD handlesAfter;
  Check(GetProcessHandleCount(GetCurrentProcess(), &handlesAfter) != 0, "handle count after");
  Check(handlesAfter <= handlesBefore + 2, "thread/event handle cleanup");
#endif

  // Reuse the same coder after all errors, also with a null progress callback.
  {
    CInput input;
    COutput output;
    Check(encoder.Code(&input, &output, NULL, NULL, NULL) == S_OK, "reuse after errors");
    Verify(input, output);
  }
  {
    CInput input;
    input.Data.clear();
    COutput output;
    Check(encoder.Code(&input, &output, NULL, NULL, NULL) == S_OK, "empty stream");
    Verify(input, output);
  }
  puts("PASS: short I/O, raw Deflate, stream/callback ownership, input/output errors, cancellation, reuse, handle cleanup, empty input");
  return 0;
}
