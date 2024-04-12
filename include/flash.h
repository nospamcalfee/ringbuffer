#ifndef _FLASH_H_
#define _FLASH_H_

int flash_read(uint32_t block, void *buffer, size_t size);
int flash_prog(uint32_t block, const void *buffer, size_t size);
int flash_erase(uint32_t block, size_t size);

#endif
