// PigzEncoder.h -- in-process parallel Deflate for the gzip handler

#ifndef ZIP7_INC_PIGZ_ENCODER_H
#define ZIP7_INC_PIGZ_ENCODER_H

#include "../../Common/MyCom.h"
#include "../ICoder.h"

namespace NCompress {
namespace NPigz {

class CEncoder Z7_final:
  public ICompressCoder,
  public ICompressSetCoderProperties,
  public CMyUnknownImp
{
  Z7_IFACES_IMP_UNK_2(ICompressCoder, ICompressSetCoderProperties)

  unsigned _level;
  unsigned _numThreads;
  size_t _blockSize;

public:
  CEncoder(): _level(5), _numThreads(1), _blockSize(128 << 10) {}
};

}}

#endif
