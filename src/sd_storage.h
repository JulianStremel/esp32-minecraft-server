// The world on a microSD card: a FAT32 card (SDMMC 1/4-bit or SPI) with one preallocated,
// contiguous file holding the world, behind a write-back cache. See sd_storage.cpp.
#pragma once
#include "mc/storage/block_device.h"

namespace mc {
class BlockDevice;
}

// Mounts the card and opens (creating it the first time) the world file. Returns the
// device to give the world store, or nullptr (logged) if there is no usable card.
// created: set when the file was just made (blank: the store may format it).
mc::BlockDevice* sdStorageOpen(bool& created);
