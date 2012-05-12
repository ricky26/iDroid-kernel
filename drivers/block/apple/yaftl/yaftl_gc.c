#include "yaftl.h"
#include "yaftl_gc.h"
#include "l2v.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
/* Internals */

static void gcPopulateBTOC(GCData* _data, uint8_t _scrub, uint32_t _max)
{
	SpareData* pSpare = sInfo.gcSpareBuffer;
	uint32_t* btocCache;
	uint32_t page;
	error_t result;

	_data->dataPagesPerSublk = sGeometry.pagesPerSublk - sInfo.tocPagesPerBlock;

	if ((btocCache = YAFTL_getBTOC(_data->chosenBlock)) != NULL) {
		// BTOC is cached, use it.
		YAFTL_copyBTOC(_data->btoc, btocCache, _max);
		return;
	}

	// Read the BTOC from the end of the block.
	page = _data->chosenBlock * sGeometry.pagesPerSublk
		+ sGeometry.pagesPerSublk - sInfo.tocPagesPerBlock;

	result = YAFTL_readBTOCPages(page, _data->btoc, pSpare, 0,
			_scrub, _max);

	if (result || !(pSpare->type & PAGETYPE_CLOSED)) {
		// Must read each page individually.
		uint32_t i;

		for (i = 0; i < sGeometry.pagesPerSublk - sInfo.tocPagesPerBlock; ++i) {
			page = _data->chosenBlock * sGeometry.pagesPerSublk + i;
			result = YAFTL_readPage(page, _data->pageBuffer1, pSpare, 0,
					1, _scrub);

			if (!result && pSpare->lpn < _max)
				_data->btoc[i] = pSpare->lpn;
		}
	}
}

static uint32_t gcListPopFront(GCList* _list)
{
	uint32_t block;
	
	if (_list->head == _list->tail)
		printk(KERN_ERR "PANIC!!!: YAFTL: gcListPopFront was called but list is empty\r\n");

	block = _list->block[_list->head++];
	if (_list->head > GCLIST_MAX)
		_list->head = 0;

	return block;
}

static int gcFillIndex(uint32_t _lpn, uint32_t* _pBuf)
{
	uint32_t basePage = sInfo.tocEntriesPerPage * _lpn;
	uint32_t i = 0, j = 0;

	sInfo.gc.index.read_c.span = 0;
	
	while (i < sInfo.tocEntriesPerPage) {
		uint32_t vpn;
		uint32_t count;

		if (sInfo.gc.index.read_c.span == 0) {
			sInfo.gc.index.read_c.pageIndex = basePage + i;
			L2V_Search(&sInfo.gc.index.read_c);

			if (sInfo.gc.index.read_c.span == 0) {
				printk(KERN_ERR "PANIC!!!: YAFTL: gcFillIndex -- no such page %d\r\n",
						basePage + i);
			}
		}

		vpn = sInfo.gc.index.read_c.vpn;
		count = MIN(sInfo.tocEntriesPerPage - i, sInfo.gc.index.read_c.span);

		if (vpn == L2V_VPN_MISS) {
			return 0;
		} else if (vpn == L2V_VPN_SPECIAL) {
			for (j = 0; j < count; ++j)
				_pBuf[i + j] = 0xFFFFFFFF;
		} else if (L2V_VPN_ISNORMAL(vpn)) {
			for (j = 0; j < count; ++j)
				_pBuf[i + j] = vpn++;

			sInfo.gc.index.read_c.vpn += count;
		} else {
			printk(KERN_ERR "PANIC!!!: YAFTL: gcFillIndex got an invalid vpn %x\r\n", vpn);
		}

		i += count;
		sInfo.gc.index.read_c.span -= count;
	}

	return 1;
}

static error_t gcReadZoneData(GCData* _data, uint8_t _isIndex, uint8_t _scrub)
{
	uint32_t* zone;
	SpareData* spareArray;
	uint8_t* pageBuffer;
	uint32_t i;

	if (_isIndex) {
		uint8_t readNeeded = 0;

		zone = sInfo.gc.index.zone;
		spareArray = sInfo.gc.index.spareArray;
		pageBuffer = sInfo.gc.index.pageBuffer2;

		for (i = 0; i < _data->curZoneSize; ++i) {
			uint32_t entry = _data->btoc[zone[i] % sGeometry.pagesPerSublk];
			spareArray[i].lpn = entry;
			if (entry == 0xFFFFFFFF) {
				printk(KERN_ERR "PANIC!!!: YAFTL: gcReadZoneData read an invalid LPN "
						"%d\r\n", entry);
			}

			if (!gcFillIndex(entry,
						(uint32_t*)&pageBuffer[i * sGeometry.bytesPerPage])) {
				readNeeded = 1;
				break;
			}
		}

		if (!readNeeded)
			return 0;
	} else {
		zone = sInfo.gc.data.zone;
		spareArray = sInfo.gc.data.spareArray;
		pageBuffer = sInfo.gc.data.pageBuffer2;
	}

	if (YAFTL_readMultiPages(zone, _data->curZoneSize, pageBuffer, spareArray,
				!_isIndex, _scrub) != 1) {
		// TODO: Don't panic, manually read the pages.
		printk(KERN_ERR "PANIC!!!: YAFTL: gcReadZoneData couldn't read multi pages\r\n");
	}

	return 0;
}

static int gcHandleVpnMiss(GCData* _data, uint8_t _scrub)
{
	uint32_t btocEntry = _data->btoc[_data->btocIdx];
	uint32_t indexPageNo = btocEntry / sInfo.tocEntriesPerPage;
	uint32_t page;
	uint32_t cache;
	SpareData* pSpare = sInfo.gcSpareBuffer;

	if (indexPageNo >= sInfo.tocArrayLength) {
		printk(KERN_ERR "PANIC!!!: YAFTL: gcHandleVpnMiss got an out-of-range index page "
				"%d\r\n", indexPageNo);
	}

	page = sInfo.tocArray[indexPageNo].indexPage;
	cache = sInfo.tocArray[indexPageNo].cacheNum;

	if (cache == 0xFFFF) {
		error_t status;

		if (page == 0xFFFFFFFF) {
			return -2;
		}

		// There is an index page, but it's not cached. Obtain it.
		cache = YAFTL_findFreeTOCCache();
		if (cache == 0xFFFF) {
			cache = YAFTL_clearEntryInCache(0xFFFF);
			if (cache == 0xFFFF) {
				_data->state = 3;
				return -1;
			}
		}

		status = YAFTL_readPage(page, (uint8_t*)sInfo.tocCaches[cache].buffer,
				pSpare, 0, 1, _scrub);

		if (status != 0) {
			if (sInfo.field_78) {
				YAFTL_writeIndexTOC();
				sInfo.field_78 = 1;
			}

			printk(KERN_ERR "PANIC!!!: YAFTL: gcHandleVpnMiss Index UECC page 0x%08x status"
					" %08x\r\n", page, status);
		} else {
			if (!(pSpare->type & PAGETYPE_INDEX)) {
				if (sInfo.field_78) {
					YAFTL_writeIndexTOC();
					sInfo.field_78 = 1;
				}

				printk(KERN_ERR "PANIC!!!: YAFTL: gcHandleVpnMiss Invalid index metadata "
						"0x%02x\r\n", pSpare->type);
			} else {
				--sInfo.numFreeCaches;
				sInfo.tocCaches[cache].state = CACHESTATE_CLEAN;
				sInfo.tocCaches[cache].useCount = 1;
				sInfo.tocCaches[cache].page = indexPageNo;
				sInfo.tocArray[indexPageNo].cacheNum = cache;
			}
		}
	}

	return sInfo.tocCaches[cache].buffer[btocEntry % sInfo.tocEntriesPerPage];
}

static void gcChooseBlock(GCData* _data, uint8_t _filter)
{
	uint32_t block = _data->victim;
	uint32_t i;
	uint32_t best = 0xFFFFFFFF;
	uint32_t bestValid = 0xFFFFFFFF;
	uint32_t bestErases = 0xFFFFFFFF;

	if (block != 0xFFFFFFFF) {
		if (sInfo.blockArray[block].status == BLOCKSTATUS_I_GC)
			YAFTL_allocateNewBlock(0);

		if (sInfo.blockArray[block].status == BLOCKSTATUS_GC)
			YAFTL_allocateNewBlock(1);
	}

	if (_data->list.head != _data->list.tail) {
		if (block != 0xFFFFFFFF)
			gcListPushBack(&_data->list, block);

		block = gcListPopFront(&_data->list);
		_data->victim = block;
	}

	if (block != 0xFFFFFFFF) {
		uint8_t status = sInfo.blockArray[block].status;

		_data->chosenBlock = block;
		_data->totalValidPages = sInfo.blockArray[block].validPagesDNo
			+ sInfo.blockArray[block].validPagesINo;
		_data->eraseCount = sInfo.blockArray[block].eraseCount;
		
		if (status != _filter) {
			if (_filter == BLOCKSTATUS_ALLOCATED
					&& status != BLOCKSTATUS_CURRENT
					&& status != BLOCKSTATUS_GC) {
				printk(KERN_ERR "PANIC!!!: YAFTL: gcChooseBlock chose a block which doesn't"
						" match the filter -- status %02x\r\n", status);
			} else if (_filter == BLOCKSTATUS_I_ALLOCATED
					&& status != BLOCKSTATUS_I_CURRENT
					&& status != BLOCKSTATUS_I_GC) {
				printk(KERN_ERR "PANIC!!!: YAFTL: gcChooseBlock chose an index block which "
						"doesn't match the filter -- status %02x\r\n", status);
			}
		}

		_data->victim = 0xFFFFFFFF;
		return;
	}

	for (i = 0; i < sGeometry.numBlocks; ++i) {
		BlockStruct* blk = &sInfo.blockArray[i];
		uint32_t valid = blk->validPagesDNo + blk->validPagesINo;

		if (blk->status == _filter && (valid < bestValid || (valid == bestValid
						&& blk->eraseCount < bestErases))) {
			best = i;
			bestValid = valid;
			bestErases = blk->eraseCount;
		}
	}

	if (best == 0xFFFFFFFF || bestValid == 0xFFFFFFFF
			|| bestErases == 0xFFFFFFFF) {
		printk(KERN_ERR "PANIC!!!: YAFTL: gcChooseBlock couldn't find a block\r\n");
	}

	_data->chosenBlock = best;
	_data->totalValidPages = bestValid;
	_data->eraseCount = bestErases;
}

static void gcSanityCheckValid(GCData* _data)
{
	BlockStruct* blk = &sInfo.blockArray[_data->chosenBlock];

	if (blk->validPagesINo + blk->validPagesDNo > 0) {
		printk(KERN_ERR "YAFTL: something could be wrong with GC, because we "
				"have %d valid index and %d valid data\r\n",
				blk->validPagesINo, blk->validPagesDNo);

		if (_data->uECC < blk->validPagesINo + blk->validPagesDNo) {
			printk(KERN_ERR "PANIC!!!: YAFTL: non-zero validity counter; block %d, "
					"uECC %d\r\n", _data->chosenBlock, _data->uECC);
		} else {
			printk(KERN_ERR "YAFTL: %d uECCs caused it. fixing up counters\r\n",
					_data->uECC);
			blk->validPagesINo = 0;
			blk->validPagesDNo = 0;
		}
	}
}

static void setupDataSpares(SpareData* _pSpare, size_t _count)
{
	uint32_t i;

	if (sInfo.lastWrittenBlock != sInfo.latestIndexBlk.blockNum) {
		sInfo.lastWrittenBlock = sInfo.latestIndexBlk.blockNum;
		++sInfo.maxIndexUsn;
	}

	for (i = 0; i < _count; ++i) {
		_pSpare[i].usn = sInfo.maxIndexUsn;
		_pSpare[i].type = (_pSpare[i].type & PAGETYPE_MAGIC) | PAGETYPE_LBN;
	}
}

static void sub_80606960(uint32_t _lpn, uint32_t _vpn)
{
	uint32_t i;

	for (i = 0; i < sInfo.unkAC_2; ++i) {
		if (sInfo.unkB4_buffer[i] == _lpn / sGeometry.pagesPerSublk)
			sInfo.unkB8_buffer[i][_lpn % sGeometry.pagesPerSublk] = _vpn;
	}
}

static int gcWriteDataPages(GCData* _data, uint8_t _scrub)
{
	uint32_t numPages = _data->curZoneSize;
	uint32_t page = 0;
	uint32_t i;
	error_t status;
	SpareData* spare = sInfo.gcSpareBuffer;

	// Write each page.
	while (numPages != 0) {
		uint32_t userPages = sGeometry.pagesPerSublk - sInfo.tocPagesPerBlock;

		if (sInfo.latestUserBlk.usedPages < userPages) {
			// There's still room in our current block. Write in it.
			uint32_t pagesToWrite = MIN(numPages,
					userPages - sInfo.latestUserBlk.usedPages);

			setupDataSpares(&sInfo.gc.data.spareArray[page], pagesToWrite);

			// Write the pages.
			if (YAFTL_writeMultiPages(
				sInfo.latestUserBlk.blockNum,
				sInfo.latestUserBlk.usedPages,
				pagesToWrite,
				&sInfo.gc.data.pageBuffer2[page * sGeometry.bytesPerPage],
				&sInfo.gc.data.spareArray[page],
				1))
			{
				gcListPushBack(&_data->list, _data->chosenBlock);
				gcListPushBack(&_data->list, sInfo.latestUserBlk.blockNum);
				YAFTL_allocateNewBlock(1);
				return ERROR_ARG;
			}

			// Update caches.
			for (i = 0; i < pagesToWrite; ++i) {
				uint32_t offsetInBlk = sInfo.latestUserBlk.usedPages + i;

				sInfo.latestUserBlk.tocBuffer[offsetInBlk] =
					sInfo.gc.data.spareArray[page + i].lpn;
				sub_80606960(sInfo.latestUserBlk.blockNum
					* sGeometry.pagesPerSublk + offsetInBlk,
					sInfo.gc.data.zone[page + i]);
				sInfo.gc.data.zone[page + i] = sInfo.latestUserBlk.blockNum
					* sGeometry.pagesPerSublk + offsetInBlk;
			}

			sInfo.latestUserBlk.usedPages += pagesToWrite;
			page += pagesToWrite;
			numPages -= pagesToWrite;
			if (numPages == 0)
				break;
		}

		if (sInfo.latestUserBlk.usedPages >= userPages) {
			// Block is filled up. Get a new one.
			YAFTL_closeLatestBlock(1);
			YAFTL_allocateNewBlock(1);
		}
	}

	// Update the TOC cache for each page.
	for (i = 0; i < _data->curZoneSize; ++i)
	{
		uint32_t indexPageNo;
		TOCStruct* toc;
		uint32_t cache;
		uint32_t tocEntry;
		uint32_t vpn;

		L2V_Update(sInfo.gc.data.spareArray[i].lpn, 1, sInfo.gc.data.zone[i]);

		indexPageNo = sInfo.gc.data.spareArray[i].lpn / sInfo.tocEntriesPerPage;
		tocEntry = sInfo.gc.data.spareArray[i].lpn % sInfo.tocEntriesPerPage;
		toc = &sInfo.tocArray[indexPageNo];
		cache = toc->cacheNum;

		if (toc->indexPage == 0xFFFFFFFF && cache == 0xFFFF) {
			printk(KERN_ERR "PANIC!!!: YAFTL: gcWriteDataPages failed to find TOC %d\r\n",
				indexPageNo);
		}

		if (cache == 0xFFFF) {
			// Need to find a new cache.
			cache = YAFTL_findFreeTOCCache();

			if (cache == 0xFFFF)
				cache = YAFTL_clearEntryInCache(0xFFFF);

			if (cache == 0xFFFF)
				printk(KERN_ERR "PANIC!!!: YAFTL: failed to find a TOC cache\r\n");

			status = YAFTL_readPage(
				toc->indexPage, (uint8_t*)sInfo.tocCaches[cache].buffer, spare,
				0, 1, _scrub);

			if (status) {
				if (sInfo.field_78) {
					YAFTL_writeIndexTOC();
					sInfo.field_78 = 0;
				}

				printk(KERN_ERR "PANIC!!!: YAFTL: uecc toc page 0x%08x status 0x%08x\r\n",
					toc->indexPage, status);
				return status;
			}

			--sInfo.numFreeCaches;
			sInfo.tocCaches[cache].useCount = 0;
		}

		vpn = sInfo.tocCaches[cache].buffer[tocEntry];

		if (vpn != 0xFFFFFFFF) {
			uint32_t blk = vpn / sGeometry.pagesPerSublk;

			if (sInfo.blockArray[blk].validPagesDNo == 0) {
				if (sInfo.field_78) {
					YAFTL_writeIndexTOC();
					sInfo.field_78 = 0;
				}

				printk(KERN_ERR "PANIC!!!: YAFTL: gcWriteDataPages tried to move a page from"
					" a block with no valid data pages %d\r\n", blk);
			}

			--sInfo.blockArray[blk].validPagesDNo;
			--sInfo.blockStats.numValidDPages;
			--sStats.dataPages;
		}

		sInfo.tocCaches[cache].buffer[tocEntry] = sInfo.gc.data.zone[i];
		++sInfo.blockArray[sInfo.gc.data.zone[i] / sGeometry.pagesPerSublk]
			.validPagesDNo;
		++sInfo.blockStats.numValidDPages;
		++sStats.dataPages;
		sInfo.tocCaches[cache].page = indexPageNo;
		toc->cacheNum = cache;

		if (toc->indexPage != 0xFFFFFFFF) {
			--sInfo.blockArray[toc->indexPage / sGeometry.pagesPerSublk]
				.validPagesINo;
			--sInfo.blockStats.numValidIPages;
			--sStats.indexPages;
			toc->indexPage = 0xFFFFFFFF;
		}

		sInfo.tocCaches[cache].state = CACHESTATE_DIRTY;
		++sInfo.tocCaches[cache].useCount;
	}

	return 0;
}

static int gcFreeDataPages(int32_t _numPages, uint8_t _scrub)
{
	while (1) {
		if (sInfo.gc.data.state == 0) {
			while (sInfo.blockStats.numIAvailable <= 1)
				gcFreeIndexPages(0xFFFFFFFF, _scrub);

			++sStats.field_20;
			gcChooseBlock(&sInfo.gc.data, BLOCKSTATUS_ALLOCATED);
			if (sInfo.gc.data.totalValidPages != 0) {
				sInfo.gc.data.btocIdx = 0;
				sInfo.gc.data.numInvalidatedPages = 0;
				gcPopulateBTOC(&sInfo.gc.data, _scrub, sInfo.totalPages);
				sInfo.blockArray[sInfo.gc.data.chosenBlock].status =
					BLOCKSTATUS_CURRENT;

				sInfo.gc.data.uECC = 0;
				sInfo.gc.data.state = 1;
				return 0;
			} else {
				++sStats.field_30;
				sInfo.gc.data.state = 2;
			}
		} else if (sInfo.gc.data.state == 1) {
			uint32_t gcPageNum;
			uint8_t continueAfterWhile = 1;

			if (sInfo.blockArray[sInfo.gc.data.chosenBlock].validPagesINo
					!= 0) {
				printk(KERN_ERR "PANIC!!!: YAFTL: gcFreeDataPages chose a block with no"
						" valid index pages\r\n");
			}

			if (sInfo.blockArray[sInfo.gc.data.chosenBlock].validPagesDNo
					== 0) {
				sInfo.gc.data.state = 2;
				continue;
			}

			// TODO: Understand this. Some sort of sorting score?
			gcPageNum = sInfo.gcPages;

			if (_numPages > 0) {
				uint32_t invalidPages = sGeometry.pagesPerSublk -
					sInfo.tocPagesPerBlock - sInfo.gc.data.totalValidPages;

				if (invalidPages == 0)
					invalidPages = 1;

				gcPageNum = sGeometry.pagesPerSublk * _numPages / invalidPages;
			}

			sInfo.gc.data.curZoneSize = 0;
			gcResetReadCache(&sInfo.gc.data.read_c);

			while (sInfo.gc.data.curZoneSize < gcPageNum &&
					sInfo.gc.data.btocIdx < sInfo.gc.data.dataPagesPerSublk &&
					sInfo.gc.data.numInvalidatedPages <
					sInfo.gc.data.totalValidPages) {
				uint32_t btocEntry =
					sInfo.gc.data.btoc[sInfo.gc.data.btocIdx];

				if (btocEntry != 0xFFFFFFFF) {
					if (sInfo.gc.data.read_c.span == 0 ||
							sInfo.gc.data.read_c.pageIndex != btocEntry) {
						uint32_t vpn;
						sInfo.gc.data.read_c.pageIndex = btocEntry;
						L2V_Search(&sInfo.gc.data.read_c);
						if (sInfo.gc.data.read_c.span == 0) {
							printk(KERN_ERR "PANIC!!!: YAFTL: gcFreeDataPages has called "
									"L2V_Search but span is still 0\r\n");
						}

						vpn = sInfo.gc.data.read_c.vpn;

						++sInfo.gc.data.read_c.pageIndex;
						--sInfo.gc.data.read_c.span;

						if (L2V_VPN_ISNORMAL(vpn)) {
							++sInfo.gc.data.read_c.vpn;
						} else if (vpn == L2V_VPN_MISS) {
							int ret = gcHandleVpnMiss(&sInfo.gc.data, _scrub);

							if (ret >= 0) {
								vpn = ret;
							} else {
								if (ret == -1)
									continueAfterWhile = 0;

								break;
							}
						} else if (vpn != L2V_VPN_SPECIAL) {
							printk(KERN_ERR "PANIC!!!: YAFTL: gcFreeDataPages doesn't know "
									"what vpn %X is\r\n", vpn);
						}

						if (vpn != L2V_VPN_SPECIAL) {
							uint32_t ourPage = sInfo.gc.data.chosenBlock
								* sGeometry.pagesPerSublk
								+ sInfo.gc.data.btocIdx;

							if (vpn == ourPage) {
								sInfo.gc.data.zone[
									sInfo.gc.data.curZoneSize++] = vpn;
								++sInfo.gc.data.numInvalidatedPages;
							}
						}
					}
				} // if (btocEntry != 0xFFFFFFFF)

				if (sInfo.gcPages - sInfo.latestUserBlk.usedPages %
						sGeometry.total_banks_ftl
						<= sInfo.gc.data.curZoneSize) {
					if (sInfo.blockArray[sInfo.gc.data.chosenBlock].
							validPagesDNo < sInfo.gc.data.curZoneSize) {
						printk(KERN_ERR "PANIC!!!: YAFTL: gcFreeDataPages tried to free "
								"more pages than it should\r\n");
					}

					if (gcReadZoneData(&sInfo.gc.data, 0, _scrub) == 0) {
						if (gcWriteDataPages(&sInfo.gc.data, _scrub) == 0) {
							sInfo.gc.data.curZoneSize = 0;
						} else {
							sInfo.gc.data.state = 4;
							continueAfterWhile = 0;
							break;
						}
					} else {
						if (sInfo.gc.data.state == 1)
							sInfo.gc.data.state = 0;

						continueAfterWhile = 0;
						break;
					}
				}

				++sInfo.gc.data.btocIdx;
			} // while

			if (!continueAfterWhile)
				break;

			// If there are still pages left in the zone, free them.
			if (sInfo.gc.data.curZoneSize > 0) {
				if (sInfo.blockArray[sInfo.gc.data.chosenBlock].validPagesDNo <
						sInfo.gc.data.curZoneSize) {
					printk(KERN_ERR "PANIC!!!: YAFTL: gcFreeDataPages had pages left in the"
							" zone, but there aren't valid pages at all\r\n");
				}

				if (gcReadZoneData(&sInfo.gc.data, 0, _scrub) == 0) {
					if (gcWriteDataPages(&sInfo.gc.data, _scrub) == 0) {
						if (sInfo.gc.data.dataPagesPerSublk <=
								sInfo.gc.data.btocIdx) {
							gcSanityCheckValid(&sInfo.gc.data);
						}

						return 0;
					} else {
						sInfo.gc.data.state = 4;
					}
				} else if (sInfo.gc.data.state == 1) {
					sInfo.gc.data.state = 0;
				}
			}
		} else if (sInfo.gc.data.state == 2) {
			// Claim this as a free block.
			uint32_t chosenBlock = sInfo.gc.data.chosenBlock;

			gcSanityCheckValid(&sInfo.gc.data);
			sInfo.blockArray[chosenBlock].unkn5 = 2;
			sInfo.blockArray[chosenBlock].readCount = 0;
			sInfo.blockArray[chosenBlock].validPagesINo = 0;
			sInfo.blockArray[chosenBlock].validPagesDNo = 0;
			sInfo.blockArray[chosenBlock].status = BLOCKSTATUS_FREE;
			++sInfo.blockStats.numFree;
			++sInfo.blockStats.numAvailable;
			--sInfo.blockStats.numAllocated;
			++sStats.freeBlocks;

			sInfo.gc.data.state = 0;
			if (sInfo.gc.data.list.head == sInfo.gc.data.list.tail)
				return 0;
		} else if (sInfo.gc.data.state == 3) {
			while (sInfo.gc.index.list.head != sInfo.gc.index.list.tail)
				gcFreeIndexPages(0xFFFFFFFF, _scrub);

			sInfo.gc.data.state = 0;
		} else if (sInfo.gc.data.state == 4) {
			sInfo.gc.data.state = 0;
		} else {
			return 0;
		}
	}

	return 0;
}

static void setupIndexSpares(SpareData* _pSpare, size_t _count)
{
	uint32_t i;
	
	if (sInfo.lastWrittenBlock != sInfo.latestIndexBlk.blockNum) {
		sInfo.lastWrittenBlock = sInfo.latestIndexBlk.blockNum;
		++sInfo.maxIndexUsn;
	}

	for (i = 0; i < _count; ++i) {
		_pSpare[i].usn = sInfo.maxIndexUsn;
		_pSpare[i].type = PAGETYPE_INDEX;
	}
}

static error_t gcWriteIndexPages(GCData* _data)
{
	SpareData* spareArray = sInfo.gc.index.spareArray;
	uint32_t block = sInfo.latestIndexBlk.blockNum;
	uint32_t leftToWrite = _data->curZoneSize;
	uint32_t pageOffset = 0;
	uint32_t i;

	while (leftToWrite > 0) {
		uint32_t usablePages = sGeometry.pagesPerSublk - sInfo.tocPagesPerBlock;

		if (sInfo.latestIndexBlk.usedPages < usablePages) {
			// Can't just close the block, because it isn't full. Bummer.
			uint32_t toWrite = MIN(leftToWrite, 
					usablePages - sInfo.latestIndexBlk.usedPages);
			uint8_t* buffer = &sInfo.gc.index.pageBuffer2[pageOffset *
				sGeometry.bytesPerPage];

			setupIndexSpares(&spareArray[pageOffset], toWrite);
			if (YAFTL_writeMultiPages(block, sInfo.latestIndexBlk.usedPages,
						toWrite, buffer, &spareArray[pageOffset], 0)) {
				// Write failure.
				gcListPushBack(&_data->list, _data->chosenBlock);
				gcListPushBack(&_data->list, block);
				YAFTL_allocateNewBlock(0);
				return EIO;
			}

			for (i = 0; i < toWrite; ++i) {
				uint32_t pageInBlk = sInfo.latestIndexBlk.usedPages + i;

				sInfo.latestIndexBlk.tocBuffer[pageInBlk] =
					spareArray[i].lpn;
				sInfo.tocArray[spareArray[i].lpn].indexPage =
					block * sGeometry.pagesPerSublk + pageInBlk;
			}
			
			sInfo.blockArray[block].validPagesINo += toWrite;
			sInfo.blockStats.numValidIPages += toWrite;
			sStats.indexPages += toWrite;
			sInfo.latestIndexBlk.usedPages += toWrite;
			pageOffset += toWrite;
			leftToWrite -= toWrite;

			if (leftToWrite == 0)
				return 0;

			if (sInfo.latestIndexBlk.usedPages + toWrite <
					sGeometry.pagesPerSublk - sInfo.tocPagesPerBlock) {
				continue;
			}
		}

		YAFTL_closeLatestBlock(0);
		YAFTL_allocateNewBlock(0);
		block = sInfo.latestIndexBlk.blockNum;

		if (sInfo.latestIndexBlk.usedPages) {
			printk(KERN_ERR "PANIC!!!: YAFTL: gcWriteIndexPages got a non-empty block "
					"%d\r\n", block);
		}
	}

	return 0;
}

/* Externals */

error_t gcInit()
{
	uint32_t i;
	for (i = sGeometry.total_banks_ftl; i <= 0xF; i <<= 1);

	gcResetReadCache(&sInfo.gc.data.read_c);
	sInfo.gcPages = i;
	sInfo.gc.data.state = 0;
	sInfo.gc.data.victim = 0xFFFFFFFF;
	sInfo.gc.index.victim = 0xFFFFFFFF;

	// Allocate buffers
	bufzone_init(&sInfo.gcBufZone);

	sInfo.gc.data.btoc = (uint32_t*)bufzone_alloc(&sInfo.gcBufZone,
			sInfo.tocPagesPerBlock * sGeometry.bytesPerPage);
	sInfo.gc.data.pageBuffer1 = (uint8_t*)bufzone_alloc(&sInfo.gcBufZone,
			sGeometry.bytesPerPage);
	sInfo.gc.index.btoc = (uint32_t*)bufzone_alloc(&sInfo.gcBufZone,
			sInfo.tocPagesPerBlock * sGeometry.bytesPerPage);
	sInfo.gc.index.pageBuffer1 = (uint8_t*)bufzone_alloc(&sInfo.gcBufZone,
			sGeometry.bytesPerPage);
	// TODO: Verify that this is working. They use something else.
	sInfo.gcSpareBuffer = (SpareData*)bufzone_alloc(&sInfo.gcBufZone,
			sizeof(SpareData));
	sInfo.gc.data.pageBuffer2 = (uint8_t*)bufzone_alloc(&sInfo.gcBufZone,
			sInfo.gcPages * sGeometry.bytesPerPage);
	sInfo.gc.index.pageBuffer2 = (uint8_t*)bufzone_alloc(&sInfo.gcBufZone,
			sInfo.gcPages * sGeometry.bytesPerPage);
	sInfo.gc.data.spareArray = (SpareData*)bufzone_alloc(&sInfo.gcBufZone,
			sInfo.gcPages * sizeof(SpareData));
	sInfo.gc.index.spareArray = (SpareData*)bufzone_alloc(&sInfo.gcBufZone,
			sInfo.gcPages * sizeof(SpareData));

	bufzone_finished_allocs(&sInfo.gcBufZone);

	sInfo.gc.data.btoc = (uint32_t*)bufzone_rebase(&sInfo.gcBufZone,
			sInfo.gc.data.btoc);
	sInfo.gc.data.pageBuffer1 = (uint8_t*)bufzone_rebase(&sInfo.gcBufZone,
			sInfo.gc.data.pageBuffer1);
	sInfo.gc.index.btoc = (uint32_t*)bufzone_rebase(&sInfo.gcBufZone,
			sInfo.gc.index.btoc);
	sInfo.gc.index.pageBuffer1 = (uint8_t*)bufzone_rebase(&sInfo.gcBufZone,
			sInfo.gc.index.pageBuffer1);
	sInfo.gcSpareBuffer = (SpareData*)bufzone_rebase(&sInfo.gcBufZone,
			sInfo.gcSpareBuffer);
	sInfo.gc.data.pageBuffer2 = (uint8_t*)bufzone_rebase(&sInfo.gcBufZone,
			sInfo.gc.data.pageBuffer2);
	sInfo.gc.index.pageBuffer2 = (uint8_t*)bufzone_rebase(&sInfo.gcBufZone,
			sInfo.gc.index.pageBuffer2);
	sInfo.gc.data.spareArray = (SpareData*)bufzone_rebase(&sInfo.gcBufZone,
			sInfo.gc.data.spareArray);
	sInfo.gc.index.spareArray = (SpareData*)bufzone_rebase(&sInfo.gcBufZone,
			sInfo.gc.index.spareArray);

	bufzone_finished_rebases(&sInfo.gcBufZone);

	sInfo.gc.data.zone = (uint32_t*)yaftl_alloc(sInfo.gcPages
			* sizeof(uint32_t));
	sInfo.gc.index.zone = (uint32_t*)yaftl_alloc(sInfo.gcPages
			* sizeof(uint32_t));
	
	if (sInfo.gc.data.btoc && sInfo.gc.data.pageBuffer1 && sInfo.gc.index.btoc
			&& sInfo.gc.index.pageBuffer1 && sInfo.gcSpareBuffer
			&& sInfo.gc.data.pageBuffer2 && sInfo.gc.index.pageBuffer2
			&& sInfo.gc.data.spareArray && sInfo.gc.index.spareArray
			&& sInfo.gc.data.zone && sInfo.gc.index.zone) {
		return 0;
	} else {
		return ENOMEM;
	}
}

void gcResetReadCache(GCReadC* _readC)
{
	_readC->field_14 = 0xFFFFFFFF;
	_readC->node = NULL;
	_readC->span = 0;
}

void gcListPushBack(GCList* _list, uint32_t _block)
{
	uint32_t curr = _list->head;

	// Search if the block already exists.
	while (curr != _list->tail) {
		if (_list->block[curr] == _block)
			return;

		++curr;
		if (curr > GCLIST_MAX)
			curr = 0;
	}

	_list->block[_list->tail] = _block;
	++_list->tail;

	if (_list->tail > GCLIST_MAX)
		_list->tail = 0;

	if (_list->tail == _list->head)
		printk(KERN_ERR "PANIC!!!: YAFTL: gcListPushBack -- list is full\r\n");
}

void gcFreeBlock(uint32_t _block, uint8_t _scrub)
{
	uint32_t chosenBlock = sInfo.gc.data.chosenBlock;
	uint32_t currBlock = _block;

	if (sInfo.gc.data.state && chosenBlock != 0xFFFFFFFF &&
			sInfo.blockArray[chosenBlock].status == BLOCKSTATUS_CURRENT) {
		sInfo.blockArray[chosenBlock].status = BLOCKSTATUS_ALLOCATED;
	}

	sInfo.gc.data.state = 0;

	while (1) {
		sInfo.gc.data.victim = currBlock;
		while (gcFreeDataPages(0, _scrub) == 0) {
			if (!sInfo.gc.data.state) {
				if (sInfo.gc.data.list.head == sInfo.gc.data.list.tail)
					return;

				currBlock = gcListPopFront(&sInfo.gc.data.list);
			}
		}
	}
}

void gcPrepareToWrite(uint32_t _numPages)
{
	uint32_t numBlocks = _numPages
		/ (sGeometry.pagesPerSublk - sInfo.tocPagesPerBlock);

	sInfo.gc.data.victim = 0xFFFFFFFF;

	while (sInfo.blockStats.numAvailable <= numBlocks + 5) {
		if (sInfo.blockStats.numIAvailable <= 1)
			gcFreeIndexPages(0xFFFFFFFF, 1);

		gcFreeBlock(0xFFFFFFFF, 1);
	}

	if (sInfo.blockStats.numAvailable <= numBlocks + 10
		|| sInfo.gc.data.state != 0)
	{
		gcFreeDataPages(_numPages, 1);
	}
}

void gcFreeIndexPages(uint32_t _victim, uint8_t _scrub)
{
	uint32_t i;

	sInfo.gc.index.victim = _victim;
	++sStats.freeIndexOps;

	while (1) {
		uint8_t failure = 0;
		uint32_t block;

		gcChooseBlock(&sInfo.gc.index, BLOCKSTATUS_I_ALLOCATED);
		block = sInfo.gc.index.chosenBlock;

		if (sInfo.gc.index.totalValidPages == 0) {
			++sStats.field_38;
		} else {
			gcPopulateBTOC(&sInfo.gc.index, _scrub, sInfo.tocArrayLength);
			sInfo.gc.index.numInvalidatedPages = 0;
			sInfo.gc.index.curZoneSize = 0;
			sInfo.gc.index.uECC = 0;
			sInfo.blockArray[sInfo.gc.index.chosenBlock].status = 0x80;

			for (i = 0; i < sInfo.gc.index.dataPagesPerSublk
					&& sInfo.gc.index.numInvalidatedPages <
					sInfo.gc.index.totalValidPages; ++i) {
				uint32_t btocEntry = sInfo.gc.index.btoc[i];
				TOCStruct* toc;
				TOCCache* tocCache = NULL;

				if(btocEntry == 0xFFFFFFFF)
					continue;

				toc = &sInfo.tocArray[btocEntry];

				// If there's no index page, or it's not our page, skip.
				if ((toc->indexPage == 0xFFFFFFFF && toc->cacheNum == 0xFFFF)
						|| (toc->indexPage != 0xFFFFFFF && toc->indexPage !=
							block * sGeometry.pagesPerSublk + i)) {
					continue;
				}

				// If the data is already cached, used it.
				if (toc->cacheNum != 0xFFFF) {
					TOCCache* cache = &sInfo.tocCaches[toc->cacheNum];
					
					if (cache->state == CACHESTATE_CLEAN) {
						// Should invalidate.
						cache->state = CACHESTATE_DIRTY;

						if (toc->indexPage == block * sGeometry.pagesPerSublk
								+ i) {
							if (sInfo.blockArray[block].validPagesINo == 0) {
								printk(KERN_ERR "PANIC!!!: YAFTL: gcFreeIndexPages tried to"
										" invalidate in block %d, with no "
										"valid pages\r\n", block);
							}

							--sInfo.blockArray[block].validPagesINo;
							--sInfo.blockStats.numValidIPages;
							--sStats.indexPages;
							++sInfo.gc.index.numInvalidatedPages;
							toc->indexPage = 0xFFFFFFFF;
						}
					}

					continue;
				}

				// Data is not cached. First, validate everything.
				if (toc->indexPage == 0xFFFFFFFF) {
					printk(KERN_ERR "PANIC!!!: YAFTL: gcFreeIndexPages found a TOC which "
							"is not cached nor available %d\r\n", block);
				}

				if (toc->indexPage != block * sGeometry.pagesPerSublk + i)
					printk(KERN_ERR "PANIC!!!: YAFTL: gcFreeIndexPages can't be here\r\n");

				// Find a free cache.
				toc->cacheNum = YAFTL_findFreeTOCCache();
				if (toc->cacheNum != 0xFFFF) {
					// Found one.
					tocCache = &sInfo.tocCaches[toc->cacheNum];
					if (YAFTL_readPage(block * sGeometry.pagesPerSublk + i,
								(uint8_t*)tocCache->buffer,
								sInfo.spareBuffer18, 0, 1, _scrub)
							!= 0) {
						// Read failure.
						failure = 1;
						break;
					}

					tocCache->state = CACHESTATE_DIRTY;
					tocCache->page = btocEntry;
					toc->indexPage = 0xFFFFFFFF;

					if (sInfo.blockArray[block].validPagesINo == 0) {
						printk(KERN_ERR "PANIC!!!: YAFTL: gcFreeIndexPages tried to "
								"invalidate an index in block %d but it has "
								"no valid indexes\r\n", block);
					}

					--sInfo.blockArray[block].validPagesINo;
					--sInfo.blockStats.numValidIPages;
					--sStats.indexPages;
					++sInfo.gc.index.numInvalidatedPages;
					continue;
				}

				// No free cache :( Must free manually.
				if (sInfo.blockArray[block].validPagesINo <
						sInfo.gc.index.curZoneSize) {
					printk(KERN_ERR "PANIC!!!: YAFTL: gcFreeIndexPages can't invalidate "
							"more pages than available (%d < %d)\r\n",
							sInfo.blockArray[block].validPagesINo,
							sInfo.gc.index.curZoneSize);
				}

				if (sInfo.gcPages - sInfo.latestIndexBlk.usedPages
						% sGeometry.total_banks_ftl
						<= sInfo.gc.index.curZoneSize) {
					// There are pages to free.
					if (gcReadZoneData(&sInfo.gc.index, 1, _scrub) != 0 ||
							gcWriteIndexPages(&sInfo.gc.index) != 0) {
						// Read / write failure.
						failure = 1;
						break;
					}

					sInfo.blockArray[block].validPagesINo -=
						sInfo.gc.index.curZoneSize;
					sInfo.blockStats.numValidIPages -=
						sInfo.gc.index.curZoneSize;
					sStats.indexPages -= sInfo.gc.index.curZoneSize;
					sInfo.gc.index.numInvalidatedPages +=
						sInfo.gc.index.curZoneSize;
					sInfo.gc.index.curZoneSize = 0;
				}

				sInfo.gc.index.zone[sInfo.gc.index.curZoneSize++] =
					block * sGeometry.pagesPerSublk + i;
			}
		}

		if (!failure) {
			if (sInfo.gc.index.curZoneSize != 0
					&& sInfo.gc.index.totalValidPages != 0) {
				// There are pages to free.
				if (gcReadZoneData(&sInfo.gc.index, 1, _scrub) != 0 ||
						gcWriteIndexPages(&sInfo.gc.index) != 0) {
					// Read / write failure.
					continue;
				}

				sInfo.blockStats.numValidIPages -= sInfo.gc.index.curZoneSize;
				sInfo.blockArray[block].validPagesINo -=
					sInfo.gc.index.curZoneSize;
				sStats.indexPages -= sInfo.gc.index.curZoneSize;
				sInfo.gc.index.numInvalidatedPages +=
					sInfo.gc.index.curZoneSize;
			}

			// Claim as a free block.
			gcSanityCheckValid(&sInfo.gc.index);
			sInfo.blockArray[block].unkn5 = 4;
			sInfo.blockArray[block].readCount = 0;
			sInfo.blockArray[block].validPagesINo = 0;
			sInfo.blockArray[block].validPagesDNo = 0;
			sInfo.blockArray[block].status = BLOCKSTATUS_FREE;
			++sInfo.blockStats.numFree;
			++sInfo.blockStats.numIAvailable;
			--sInfo.blockStats.numIAllocated;
			++sStats.freeBlocks;

			if (sInfo.gc.index.list.head == sInfo.gc.index.list.tail)
				return;
		}
	}
}

void gcPrepareToFlush()
{
	uint32_t i;

	while (sInfo.numIBlocks < sInfo.blockStats.numIAllocated + 2)
		gcFreeIndexPages(0xFFFFFFFF, 1);

	for (i = 0; i < sInfo.numCaches; ++i)
		YAFTL_clearEntryInCache(i);
}
