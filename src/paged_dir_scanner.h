// paged_dir_scanner.h — Shared 32-entry sliding-window SD directory & list scanner helpers
#pragma once
#include <Arduino.h>
#include <SD.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "storage.h"
#include "ui.h"

namespace looper {

static inline bool pagedDirNameLess(const char *aName, uint8_t aIsDir, const char *bName, uint8_t bIsDir) {
  const bool aParent = (aIsDir != 0) && (strcmp(aName, "..") == 0);
  const bool bParent = (bIsDir != 0) && (strcmp(bName, "..") == 0);
  if (aParent != bParent) return aParent;
  if ((aIsDir != 0) != (bIsDir != 0)) return aIsDir != 0;
  const int cmp = strcasecmp(aName, bName);
  if (cmp != 0) return cmp < 0;
  return strcmp(aName, bName) < 0;
}

template <typename Entry>
FLASHMEM static inline bool pagedDirEntryLessFn(const Entry &a, const Entry &b) {
  return pagedDirNameLess(a.name, a.isDir, b.name, b.isDir);
}

template <typename Entry, typename LessFn>
FLASHMEM static void pagedInsertSmallest(Entry *arr, int &count, int cap, const Entry &item, LessFn less) {
  if (cap <= 0) return;
  if (count >= cap && !less(item, arr[cap - 1])) return;
  int ins = (count < cap) ? count++ : (cap - 1);
  while (ins > 0 && less(item, arr[ins - 1])) {
    arr[ins] = arr[ins - 1];
    ins--;
  }
  arr[ins] = item;
}

template <typename Entry, typename LessFn>
FLASHMEM static void pagedInsertLargest(Entry *arr, int &count, int cap, const Entry &item, LessFn less) {
  if (cap <= 0) return;
  if (count >= cap && !less(arr[0], item)) return;
  if (count < cap) {
    int ins = count++;
    while (ins > 0 && less(item, arr[ins - 1])) {
      arr[ins] = arr[ins - 1];
      ins--;
    }
    arr[ins] = item;
  } else {
    int ins = 0;
    while (ins + 1 < cap && less(arr[ins + 1], item)) {
      arr[ins] = arr[ins + 1];
      ins++;
    }
    arr[ins] = item;
  }
}

template <typename Entry>
FLASHMEM static void pagedFinalizeWindow(Entry *entries, bool usePivot, const Entry &pivotCopy,
                                  int pivotIdx, int pOff, int leftCount, int rightCount,
                                  int totalCount, int &outTotalCount,
                                  int &outWinStart, int &outWinCount) {
  outTotalCount = totalCount;
  if (!usePivot) {
    outWinStart = 0;
    outWinCount = rightCount;
  } else {
    entries[leftCount] = pivotCopy;
    if (leftCount < pOff && rightCount > 0) {
      memmove(&entries[leftCount + 1], &entries[pOff + 1],
              (size_t)rightCount * sizeof(Entry));
    }
    outWinStart = max(0, pivotIdx - leftCount);
    outWinCount = leftCount + 1 + rightCount;
  }
}

template <typename Entry>
FLASHMEM static void pagedScanDirWindow(const char *dirPath, Entry *entries, int winCap,
                                 int &outTotalCount, int &outWinStart, int &outWinCount,
                                 bool (*acceptFile)(const char *name, uint64_t fSize),
                                 const Entry *pivot = nullptr, int pivotIdx = 0, int wantStart = 0,
                                 const char *matchFile = nullptr, int *outMatchIdx = nullptr,
                                 Entry *outMatchEntry = nullptr) {
  if (outMatchIdx) *outMatchIdx = -1;

  Entry pivotCopy{};
  const bool usePivot = (pivot != nullptr);
  int pOff = 0, leftCap = 0, rightCap = winCap;
  int leftCount = 0, rightCount = 0, totalCount = 0;

  if (usePivot) {
    pivotCopy = *pivot;
    pOff = constrain(pivotIdx - wantStart, 0, winCap - 1);
    leftCap = pOff;
    rightCap = (winCap - 1) - pOff;
  }

  Entry matchTarget{};
  const bool hasMatchTarget = (matchFile != nullptr && matchFile[0] != '\0' &&
                               strlen(matchFile) < sizeof(matchTarget.name));
  int matchRank = 0;
  bool matchFound = false;
  if (hasMatchTarget) {
    snprintf(matchTarget.name, sizeof(matchTarget.name), "%s", matchFile);
    matchTarget.isDir = 0;
  }

  auto considerItem = [&](const Entry &item) FLASHMEM {
    totalCount++;
    if (hasMatchTarget) {
      if (!item.isDir && strcasecmp(item.name, matchTarget.name) == 0) {
        matchFound = true;
        if (outMatchEntry) *outMatchEntry = item;
      } else if (pagedDirEntryLessFn(item, matchTarget)) {
        matchRank++;
      }
    }
    if (!usePivot) {
      pagedInsertSmallest(entries, rightCount, rightCap, item, pagedDirEntryLessFn<Entry>);
    } else if (pagedDirEntryLessFn(item, pivotCopy)) {
      pagedInsertLargest(entries, leftCount, leftCap, item, pagedDirEntryLessFn<Entry>);
    } else if (pagedDirEntryLessFn(pivotCopy, item)) {
      pagedInsertSmallest(&entries[pOff + 1], rightCount, rightCap, item, pagedDirEntryLessFn<Entry>);
    }
  };

  if (strcmp(dirPath, "/") != 0) {
    Entry parentItem{};
    snprintf(parentItem.name, sizeof(parentItem.name), "..");
    parentItem.isDir = 1;
    considerItem(parentItem);
  }

  FsFile dir = SD.sdfs.open(dirPath, O_RDONLY);
  if (dir.isOpen() && dir.isDir()) {
    FsFile entry;
    while (entry.openNext(&dir, O_RDONLY)) {
      char nameBuf[64] = {};
      entry.getName(nameBuf, sizeof(nameBuf));
      bool isSubDir = entry.isDir();
      uint64_t fSize = isSubDir ? 0 : entry.fileSize();
      bool isHidden = entry.isHidden() || (nameBuf[0] == '.') ||
                      (strcasecmp(nameBuf, "System Volume Information") == 0);
      entry.close();

      if (isHidden || nameBuf[0] == '\0') continue;
      if (!isSubDir && (!acceptFile || !acceptFile(nameBuf, fSize))) continue;
      if (strlen(nameBuf) >= sizeof(entries[0].name)) continue;

      Entry item{};
      snprintf(item.name, sizeof(item.name), "%s", nameBuf);
      item.isDir = isSubDir ? 1 : 0;
      considerItem(item);
      keepStreaming();
    }
  }
  if (dir.isOpen()) dir.close();

  pagedFinalizeWindow(entries, usePivot, pivotCopy, pivotIdx, pOff, leftCount, rightCount,
                      totalCount, outTotalCount, outWinStart, outWinCount);
  if (matchFound && outMatchIdx) {
    *outMatchIdx = matchRank;
  }
}

template <typename Entry>
FLASHMEM static void pagedSyncWindowScroll(int totalCount, int visRows, int winCap,
                                    int &cur, int &scr, int &winStart, int &winCount,
                                    const Entry *winEntries,
                                    void (*rescan)(const Entry *pivot, int pivotIdx, int wantStart)) {
  if (totalCount <= 0) {
    cur = 0;
    scr = 0;
    winStart = 0;
    winCount = 0;
    return;
  }
  cur = constrain(cur, 0, totalCount - 1);
  if (cur < scr) {
    scr = cur;
  } else if (cur >= scr + visRows) {
    scr = cur - visRows + 1;
  }
  int maxScroll = max(0, totalCount - visRows);
  scr = constrain(scr, 0, maxScroll);

  int visEnd = min(totalCount - 1, scr + visRows - 1);
  if (scr < winStart || visEnd >= winStart + winCount) {
    int maxWinStart = max(0, totalCount - winCap);
    int wantStart = constrain(scr - 12, 0, maxWinStart);
    if (wantStart == 0) {
      rescan(nullptr, 0, 0);
    } else {
      int overlapLo = max(winStart, wantStart);
      int overlapHi = min(winStart + winCount - 1, wantStart + winCap - 1);
      if (winCount > 0 && overlapLo <= overlapHi) {
        int pivotIdx = (wantStart < winStart) ? overlapHi : overlapLo;
        Entry pivot = winEntries[pivotIdx - winStart];
        rescan(&pivot, pivotIdx, wantStart);
      } else {
        rescan(nullptr, 0, 0);
      }
    }
  }
}

FLASHMEM static inline bool pagedStepBrowserDir(char *dirBuf, size_t dirCap, const char *entName) {
  if (strcmp(entName, "..") == 0) {
    char *lastSlash = strrchr(dirBuf, '/');
    if (!lastSlash || lastSlash == dirBuf) {
      snprintf(dirBuf, dirCap, "/");
    } else {
      *lastSlash = '\0';
    }
    return true;
  }
  const size_t dLen = (strcmp(dirBuf, "/") == 0) ? 0u : strlen(dirBuf);
  const size_t nLen = strlen(entName);
  if (dLen + 1u + nLen >= dirCap) {
    uiShowToast("PATH TOO LONG");
    uiForceRedraw();
    return false;
  }
  dirBuf[dLen] = '/';
  memcpy(dirBuf + dLen + 1u, entName, nLen + 1u);
  return true;
}

}  // namespace looper
