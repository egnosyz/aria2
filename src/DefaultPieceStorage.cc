/* <!-- copyright */
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2006 Tatsuhiro Tsujikawa
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 */
/* copyright --> */
#include "DefaultPieceStorage.h"
#include "DownloadContext.h"
#include "Piece.h"
#include "Peer.h"
#include "LogFactory.h"
#include "Logger.h"
#include "prefs.h"
#include "DirectDiskAdaptor.h"
#include "MultiDiskAdaptor.h"
#include "CopyDiskAdaptor.h"
#include "DiskWriter.h"
#include "BitfieldManFactory.h"
#include "BitfieldMan.h"
#include "message.h"
#include "DefaultDiskWriterFactory.h"
#include "FileEntry.h"
#include "DlAbortEx.h"
#include "Util.h"
#include "a2functional.h"
#include "Option.h"
#include "StringFormat.h"
#include <numeric>
#include <algorithm>

namespace aria2 {

class GenPieceStat {
private:
  size_t _index;
public:
  GenPieceStat():_index(0) {}

  SharedHandle<PieceStat> operator()()
  {
    return SharedHandle<PieceStat>(new PieceStat(_index++));
  }
};

class PieceRarer
{
public:
  bool operator()(const SharedHandle<PieceStat>& left,
		  const SharedHandle<PieceStat>& right)
  {
    if(left->getCount() == right->getCount()) {
      return left->getOrder() < right->getOrder();
    } else {
      return left->getCount() < right->getCount();
    }
  }
};

DefaultPieceStorage::DefaultPieceStorage(const DownloadContextHandle& downloadContext, const Option* option):
  downloadContext(downloadContext),
  _diskWriterFactory(new DefaultDiskWriterFactory()),
  endGamePieceNum(END_GAME_PIECE_NUM),
  option(option),
  _pieceStats(downloadContext->getNumPieces())
{
  bitfieldMan =
    BitfieldManFactory::getFactoryInstance()->
    createBitfieldMan(downloadContext->getPieceLength(),
		      downloadContext->getTotalLength());

  std::generate(_pieceStats.begin(), _pieceStats.end(), GenPieceStat());
  _sortedPieceStats = _pieceStats;
  // we need some randomness in ordering.
  std::random_shuffle(_sortedPieceStats.begin(), _sortedPieceStats.end());
  {
    size_t order = 0;
    for(std::deque<SharedHandle<PieceStat> >::iterator i = _sortedPieceStats.begin();
	i != _sortedPieceStats.end(); ++i) {
      (*i)->setOrder(order++);
    }
  }
  std::sort(_sortedPieceStats.begin(), _sortedPieceStats.end(), PieceRarer());

  logger = LogFactory::getInstance();
}

DefaultPieceStorage::~DefaultPieceStorage() {
  delete bitfieldMan;
}

bool DefaultPieceStorage::hasMissingPiece(const PeerHandle& peer)
{
  return bitfieldMan->hasMissingPiece(peer->getBitfield(),
				      peer->getBitfieldLength());
}

bool DefaultPieceStorage::isEndGame()
{
  return bitfieldMan->countMissingBlock() <= endGamePieceNum;
}

class FindRarestPiece
{
private:
  const std::deque<size_t>& _indexes;
public:
  FindRarestPiece(const std::deque<size_t>& indexes):_indexes(indexes) {}

  bool operator()(const SharedHandle<PieceStat>& pieceStat)
  {
    return std::binary_search(_indexes.begin(), _indexes.end(), pieceStat->getIndex());
  }
};

bool DefaultPieceStorage::getMissingPieceIndex(size_t& index, const PeerHandle& peer)
{
  std::deque<size_t> indexes;
  bool r;
  if(isEndGame()) {
    r = bitfieldMan->getAllMissingIndexes(indexes, peer->getBitfield(),
					  peer->getBitfieldLength());
  } else {
    r = bitfieldMan->getAllMissingUnusedIndexes(indexes,
						peer->getBitfield(),
						peer->getBitfieldLength());
  }
  if(r) {
    // We assume indexes is sorted using comparator less.
    //std::sort(indexes.begin(), indexes.end());
    std::deque<SharedHandle<PieceStat> >::const_iterator i =
      std::find_if(_sortedPieceStats.begin(), _sortedPieceStats.end(),
		   FindRarestPiece(indexes));
    index = (*i)->getIndex();
    return true;
  } else {
    return false;
  }
}

PieceHandle DefaultPieceStorage::checkOutPiece(size_t index)
{
  bitfieldMan->setUseBit(index);

  PieceHandle piece = findUsedPiece(index);
  if(piece.isNull()) {
    piece.reset(new Piece(index, bitfieldMan->getBlockLength(index)));
    addUsedPiece(piece);
    return piece;
  } else {
    return piece;
  }
}

/**
 * Newly instantiated piece is not added to usedPieces.
 * Because it is waste of memory and there is no chance to use them later.
 */
PieceHandle DefaultPieceStorage::getPiece(size_t index)
{
  PieceHandle piece;
  if(0 <= index && index <= bitfieldMan->getMaxIndex()) {
    piece = findUsedPiece(index);
    if(piece.isNull()) {
      piece.reset(new Piece(index, bitfieldMan->getBlockLength(index)));
      if(hasPiece(index)) {
	piece->setAllBlock();
      }
    }
  }
  return piece;
}

void DefaultPieceStorage::addUsedPiece(const PieceHandle& piece)
{
  usedPieces.push_back(piece);
}

class FindPiece {
private:
  size_t index;
public:
  FindPiece(size_t index):index(index) {}

  bool operator()(const PieceHandle& piece) {
    return piece->getIndex() == index;
  }
};

PieceHandle DefaultPieceStorage::findUsedPiece(size_t index) const
{
  Pieces::const_iterator itr = std::find_if(usedPieces.begin(),
					    usedPieces.end(),
					    FindPiece(index));
  if(itr == usedPieces.end()) {
    return SharedHandle<Piece>();
  } else {
    return *itr;
  }
}

PieceHandle DefaultPieceStorage::getMissingPiece(const PeerHandle& peer)
{
  size_t index;
  if(getMissingPieceIndex(index, peer)) {
    return checkOutPiece(index);
  } else {
    return SharedHandle<Piece>();
  }
}

bool DefaultPieceStorage::getMissingFastPieceIndex(size_t& index,
						   const PeerHandle& peer)
{
  if(peer->isFastExtensionEnabled() && peer->countPeerAllowedIndexSet() > 0) {
    BitfieldMan tempBitfield(bitfieldMan->getBlockLength(),
			     bitfieldMan->getTotalLength());
    for(std::deque<size_t>::const_iterator itr = peer->getPeerAllowedIndexSet().begin();
	itr != peer->getPeerAllowedIndexSet().end(); itr++) {
      if(!bitfieldMan->isBitSet(index) && peer->hasPiece(*itr)) {
	tempBitfield.setBit(*itr);
      }
    }
    if(isEndGame()) {
      return bitfieldMan->getMissingIndex(index, tempBitfield.getBitfield(),
					  tempBitfield.getBitfieldLength());
    } else {
      return bitfieldMan->getMissingUnusedIndex(index,
						tempBitfield.getBitfield(),
						tempBitfield.getBitfieldLength());
    }
  }
  return false;
}

PieceHandle DefaultPieceStorage::getMissingFastPiece(const PeerHandle& peer)
{
  size_t index;
  if(getMissingFastPieceIndex(index, peer)) {
    return checkOutPiece(index);
  } else {
    return SharedHandle<Piece>();
  }
}

PieceHandle DefaultPieceStorage::getMissingPiece()
{
  size_t index;
  if(bitfieldMan->getSparseMissingUnusedIndex(index)) {
    return checkOutPiece(index);
  } else {
    return SharedHandle<Piece>();
  }
}

PieceHandle DefaultPieceStorage::getMissingPiece(size_t index)
{
  if(hasPiece(index) || isPieceUsed(index)) {
    return SharedHandle<Piece>();
  } else {
    return checkOutPiece(index);
  }
}

void DefaultPieceStorage::deleteUsedPiece(const PieceHandle& piece)
{
  if(piece.isNull()) {
    return;
  }
  Pieces::iterator itr = std::find(usedPieces.begin(), usedPieces.end(), piece);
  if(itr != usedPieces.end()) {
    usedPieces.erase(itr);
  }
}

void DefaultPieceStorage::reduceUsedPieces(size_t delMax)
{
  if(usedPieces.size() <= delMax) {
    return;
  }
  size_t toDelete = usedPieces.size()-delMax;
  int fillRate = 10;
  while(fillRate < 50) {
    size_t deleted = deleteUsedPiecesByFillRate(fillRate, toDelete);
    if(deleted == 0) {
      break;
    }
    toDelete -= deleted;
    fillRate += 10;
  }
}

size_t DefaultPieceStorage::deleteUsedPiecesByFillRate(int fillRate,
						       size_t toDelete)
{
  size_t deleted = 0;
  for(Pieces::iterator itr = usedPieces.begin();
      itr != usedPieces.end() && deleted < toDelete;) {
    PieceHandle& piece = *itr;
    if(!bitfieldMan->isUseBitSet(piece->getIndex()) &&
       piece->countCompleteBlock() <= piece->countBlock()*(fillRate/100.0)) {
      logger->debug(MSG_DELETING_USED_PIECE,
		    piece->getIndex(),
		    (piece->countCompleteBlock()*100)/piece->countBlock(),
		    fillRate);
      itr = usedPieces.erase(itr);
      deleted++;
    } else {
      itr++;
    }
  }
  return deleted;
}

void DefaultPieceStorage::completePiece(const PieceHandle& piece)
{
  if(piece.isNull()) {
    return;
  }
  deleteUsedPiece(piece);
  if(!isEndGame()) {
    reduceUsedPieces(100);
  }
  if(allDownloadFinished()) {
    return;
  }
  bitfieldMan->setBit(piece->getIndex());
  bitfieldMan->unsetUseBit(piece->getIndex());
  addPieceStats(piece->getIndex());
  if(downloadFinished()) {
    diskAdaptor->onDownloadComplete();
    if(isSelectiveDownloadingMode()) {
      logger->notice(MSG_SELECTIVE_DOWNLOAD_COMPLETED);
      // following line was commented out in order to stop sending request
      // message after user-specified files were downloaded.
      //finishSelectiveDownloadingMode();
    } else {
      logger->info(MSG_DOWNLOAD_COMPLETED);
    }
  }
}

bool DefaultPieceStorage::isSelectiveDownloadingMode()
{
  return bitfieldMan->isFilterEnabled();
}

void DefaultPieceStorage::finishSelectiveDownloadingMode()
{
  bitfieldMan->clearFilter();
  diskAdaptor->addAllDownloadEntry();
}

// not unittested
void DefaultPieceStorage::cancelPiece(const PieceHandle& piece)
{
  if(piece.isNull()) {
    return;
  }
  bitfieldMan->unsetUseBit(piece->getIndex());
  if(!isEndGame()) {
    if(piece->getCompletedLength() == 0) {
      deleteUsedPiece(piece);
    }
  }
}

bool DefaultPieceStorage::hasPiece(size_t index)
{
  return bitfieldMan->isBitSet(index);
}

bool DefaultPieceStorage::isPieceUsed(size_t index)
{
  return bitfieldMan->isUseBitSet(index);
}

uint64_t DefaultPieceStorage::getTotalLength()
{
  return bitfieldMan->getTotalLength();
}

uint64_t DefaultPieceStorage::getFilteredTotalLength()
{
  return bitfieldMan->getFilteredTotalLength();
}

uint64_t DefaultPieceStorage::getCompletedLength()
{
  return bitfieldMan->getCompletedLength()+getInFlightPieceCompletedLength();
}

uint64_t DefaultPieceStorage::getFilteredCompletedLength()
{
  return bitfieldMan->getFilteredCompletedLength()+getInFlightPieceCompletedLength();
}

size_t DefaultPieceStorage::getInFlightPieceCompletedLength() const
{
  return std::accumulate(usedPieces.begin(), usedPieces.end(), 0, adopt2nd(std::plus<size_t>(), mem_fun_sh(&Piece::getCompletedLength)));
}

// not unittested
void DefaultPieceStorage::setFileFilter(const std::deque<std::string>& filePaths)
{
  if(downloadContext->getFileMode() != DownloadContext::MULTI || filePaths.empty()) {
    return;
  }
  diskAdaptor->removeAllDownloadEntry();
  for(std::deque<std::string>::const_iterator pitr = filePaths.begin();
      pitr != filePaths.end(); pitr++) {
    if(!diskAdaptor->addDownloadEntry(*pitr)) {
      throw DlAbortEx(StringFormat(EX_NO_SUCH_FILE_ENTRY, (*pitr).c_str()).str());
    }
    FileEntryHandle fileEntry = diskAdaptor->getFileEntryFromPath(*pitr);
    bitfieldMan->addFilter(fileEntry->getOffset(), fileEntry->getLength());
  }
  bitfieldMan->enableFilter();
}

void DefaultPieceStorage::setFileFilter(IntSequence seq)
{
  std::deque<int32_t> fileIndexes = seq.flush();
  // TODO Is sorting necessary?
  std::sort(fileIndexes.begin(), fileIndexes.end());
  fileIndexes.erase(std::unique(fileIndexes.begin(), fileIndexes.end()), fileIndexes.end());
  std::deque<std::string> filePaths;
  const FileEntries& entries = diskAdaptor->getFileEntries();
  for(size_t i = 0; i < entries.size(); i++) {
    if(std::find(fileIndexes.begin(), fileIndexes.end(), i+1) != fileIndexes.end()) {
      logger->debug("index=%d is %s", i+1, entries[i]->getPath().c_str());
      filePaths.push_back(entries[i]->getPath());
    }
  }
  setFileFilter(filePaths);
}

// not unittested
void DefaultPieceStorage::clearFileFilter()
{
  bitfieldMan->clearFilter();
  diskAdaptor->addAllDownloadEntry();
}

// not unittested
bool DefaultPieceStorage::downloadFinished()
{
  // TODO iterate all requested FileEntry and Call bitfieldMan->isBitSetOffsetRange()
  return bitfieldMan->isFilteredAllBitSet();
}

// not unittested
bool DefaultPieceStorage::allDownloadFinished()
{
  return bitfieldMan->isAllBitSet();
}

// not unittested
void DefaultPieceStorage::initStorage()
{
  if(downloadContext->getFileMode() == DownloadContext::SINGLE) {
    logger->debug("Instantiating DirectDiskAdaptor");
    DiskWriterHandle writer = _diskWriterFactory->newDiskWriter();
    writer->setDirectIOAllowed(option->getAsBool(PREF_ENABLE_DIRECT_IO));
    DirectDiskAdaptorHandle directDiskAdaptor(new DirectDiskAdaptor());
    directDiskAdaptor->setDiskWriter(writer);
    directDiskAdaptor->setTotalLength(downloadContext->getTotalLength());
    this->diskAdaptor = directDiskAdaptor;
  } else {
    // file mode == DownloadContext::MULTI
    if(option->get(PREF_DIRECT_FILE_MAPPING) == V_TRUE) {
      logger->debug("Instantiating MultiDiskAdaptor");
      MultiDiskAdaptorHandle multiDiskAdaptor(new MultiDiskAdaptor());
      multiDiskAdaptor->setDirectIOAllowed(option->getAsBool(PREF_ENABLE_DIRECT_IO));
      multiDiskAdaptor->setPieceLength(downloadContext->getPieceLength());
      multiDiskAdaptor->setTopDir(downloadContext->getName());
      this->diskAdaptor = multiDiskAdaptor;
    } else {
      logger->debug("Instantiating CopyDiskAdaptor");
      DiskWriterHandle writer = _diskWriterFactory->newDiskWriter();
      writer->setDirectIOAllowed(option->getAsBool(PREF_ENABLE_DIRECT_IO));
      CopyDiskAdaptorHandle copyDiskAdaptor(new CopyDiskAdaptor());
      copyDiskAdaptor->setDiskWriter(writer);
      copyDiskAdaptor->setTempFilename(downloadContext->getName()+".a2tmp");
      copyDiskAdaptor->setTotalLength(downloadContext->getTotalLength());
      if(downloadContext->getFileMode() == DownloadContext::MULTI) {
	copyDiskAdaptor->setTopDir(downloadContext->getName());
      }
      this->diskAdaptor = copyDiskAdaptor;
    }
  }
  diskAdaptor->setStoreDir(downloadContext->getDir());
  diskAdaptor->setFileEntries(downloadContext->getFileEntries());
}

void DefaultPieceStorage::setBitfield(const unsigned char* bitfield,
				      size_t bitfieldLength)
{
  bitfieldMan->setBitfield(bitfield, bitfieldLength);
  addPieceStats(bitfield, bitfieldLength);
}

size_t DefaultPieceStorage::getBitfieldLength()
{
  return bitfieldMan->getBitfieldLength();
}

const unsigned char* DefaultPieceStorage::getBitfield()
{
  return bitfieldMan->getBitfield();
}

DiskAdaptorHandle DefaultPieceStorage::getDiskAdaptor() {
  return diskAdaptor;
}

size_t DefaultPieceStorage::getPieceLength(size_t index)
{
  return bitfieldMan->getBlockLength(index);
}

void DefaultPieceStorage::advertisePiece(int32_t cuid, size_t index)
{
  HaveEntry entry(cuid, index);
  haves.push_front(entry);
}

std::deque<size_t>
DefaultPieceStorage::getAdvertisedPieceIndexes(int32_t myCuid,
					       const Time& lastCheckTime)
{
  std::deque<size_t> indexes;
  for(Haves::const_iterator itr = haves.begin(); itr != haves.end(); itr++) {
    const Haves::value_type& have = *itr;
    if(have.getCuid() == myCuid) {
      continue;
    }
    if(lastCheckTime.isNewer(have.getRegisteredTime())) {
      break;
    }
    indexes.push_back(have.getIndex());
  }
  return indexes;
}

class FindElapsedHave
{
private:
  time_t elapsed;
public:
  FindElapsedHave(time_t elapsed):elapsed(elapsed) {}

  bool operator()(const HaveEntry& have) {
    if(have.getRegisteredTime().elapsed(elapsed)) {
      return true;
    } else {
      return false;
    }
  }
};
  
void DefaultPieceStorage::removeAdvertisedPiece(time_t elapsed)
{
  Haves::iterator itr =
    std::find_if(haves.begin(), haves.end(), FindElapsedHave(elapsed));
  if(itr != haves.end()) {
    logger->debug(MSG_REMOVED_HAVE_ENTRY, haves.end()-itr);
    haves.erase(itr, haves.end());
  }
}

void DefaultPieceStorage::markAllPiecesDone()
{
  bitfieldMan->setAllBit();
}

void DefaultPieceStorage::markPiecesDone(uint64_t length)
{
  if(length == bitfieldMan->getTotalLength()) {
    bitfieldMan->setAllBit();
  } else {
    size_t numPiece = length/bitfieldMan->getBlockLength();
    if(numPiece > 0) {
      bitfieldMan->setBitRange(0, numPiece-1);
    }
    size_t r = (length%bitfieldMan->getBlockLength())/Piece::BLOCK_LENGTH;
    if(r > 0) {
      PieceHandle p(new Piece(numPiece, bitfieldMan->getBlockLength(numPiece)));
      for(size_t i = 0; i < r; ++i) {
	p->completeBlock(i);
      }
      addUsedPiece(p);
    }
  }
}

void DefaultPieceStorage::markPieceMissing(size_t index)
{
  bitfieldMan->unsetBit(index);
}

void DefaultPieceStorage::addInFlightPiece(const Pieces& pieces)
{
  std::copy(pieces.begin(), pieces.end(), std::back_inserter(usedPieces));
}

size_t DefaultPieceStorage::countInFlightPiece()
{
  return usedPieces.size();
}

Pieces DefaultPieceStorage::getInFlightPieces()
{
  return usedPieces;
}

void DefaultPieceStorage::setDiskWriterFactory(const DiskWriterFactoryHandle& diskWriterFactory)
{
  _diskWriterFactory = diskWriterFactory;
}

void DefaultPieceStorage::addPieceStats(const unsigned char* bitfield,
					size_t bitfieldLength)
{
  size_t index = 0;
  for(size_t bi = 0; bi < bitfieldLength; ++bi) {
    
    for(size_t i = 0; i < 8; ++i, ++index) {
      unsigned char mask = 128 >> i;
      if(bitfield[bi]&mask) {
	_pieceStats[index]->addCount();
      }
    }

  }
  std::sort(_sortedPieceStats.begin(), _sortedPieceStats.end(), PieceRarer());
}

void DefaultPieceStorage::subtractPieceStats(const unsigned char* bitfield,
					     size_t bitfieldLength)
{
  size_t index = 0;
  for(size_t bi = 0; bi < bitfieldLength; ++bi) {
    
    for(size_t i = 0; i < 8; ++i, ++index) {
      unsigned char mask = 128 >> i;
      if(bitfield[bi]&mask) {
	_pieceStats[index]->subCount();
      }
    }

  }
  std::sort(_sortedPieceStats.begin(), _sortedPieceStats.end(), PieceRarer());
}

void DefaultPieceStorage::updatePieceStats(const unsigned char* newBitfield,
					   size_t newBitfieldLength,
					   const unsigned char* oldBitfield)
{
  size_t index = 0;
  for(size_t bi = 0; bi < newBitfieldLength; ++bi) {
    
    for(size_t i = 0; i < 8; ++i, ++index) {
      unsigned char mask = 128 >> i;
      if((newBitfield[bi]&mask) && !(oldBitfield[bi]&mask)) {
	_pieceStats[index]->addCount();
      } else if(!(newBitfield[bi]&mask) && (oldBitfield[bi]&mask)) {
	_pieceStats[index]->subCount();
      }
    }

  }
  std::sort(_sortedPieceStats.begin(), _sortedPieceStats.end(), PieceRarer());
}

void DefaultPieceStorage::addPieceStats(size_t index)
{
  std::deque<SharedHandle<PieceStat> >::iterator cur =
    std::lower_bound(_sortedPieceStats.begin(), _sortedPieceStats.end(),
		     _pieceStats[index], PieceRarer());

  (*cur)->addCount();

  std::deque<SharedHandle<PieceStat> >::iterator last =
    std::upper_bound(cur+1, _sortedPieceStats.end(), *cur, PieceRarer());

  std::sort(cur, last, PieceRarer());
//   for(std::deque<SharedHandle<PieceStat> >::const_iterator i = _sortedPieceStats.begin(); i != _sortedPieceStats.end(); ++i) {
//     logger->debug("index = %u, count = %u", (*i)->getIndex(), (*i)->getCount());
//   }
}

PieceStat::PieceStat(size_t index):_order(0), _index(index), _count(0) {}

void PieceStat::addCount()
{
  if(_count < SIZE_MAX) {
    ++_count;
  }
}

void PieceStat::subCount()
{
  if(_count > 0) {
    --_count;
  }
}

size_t PieceStat::getIndex() const
{
  return _index;
}

size_t PieceStat::getCount() const
{
  return _count;
}

void PieceStat::setOrder(size_t order)
{
  _order = order;
}

size_t PieceStat::getOrder() const
{
  return _order;
}

} // namespace aria2
