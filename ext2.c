#include "ext2.h"

#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/fsuid.h>
#include <stdint.h>
#include <sys/stat.h>
#include <fcntl.h>

#define EXT2_OFFSET_SUPERBLOCK 1024
#define EXT2_INVALID_BLOCK_NUMBER ((uint32_t) -1)

/* open_volume_file: Opens the specified file and reads the initial
   EXT2 data contained in the file, including the boot sector, file
   allocation table and root directory.
   
   Parameters:
     filename: Name of the file containing the volume data.
   Returns:
     A pointer to a newly allocated volume_t data structure with all
     fields initialized according to the data in the volume file
     (including superblock and group descriptor table), or NULL if the
     file is invalid or data is missing, or if the file is not an EXT2
     volume file system (s_magic does not contain the correct value).
 */
volume_t *open_volume_file(const char *filename) {
  int fd = open(filename, O_RDONLY);
  //if fd is invalid return NULL
  if (fd < 0) {
    return NULL;
  }
  //initialize volume object, superblock and set volume fd
  volume_t *vol = malloc(sizeof(volume_t));
  superblock_t sb;
  vol->fd = fd;
  
  //if there is not enough data for the super block in the file, return NULL
  if (pread(vol->fd, &sb, sizeof(superblock_t), EXT2_OFFSET_SUPERBLOCK) < sizeof(superblock_t)) {
    return NULL;
  }

  //if the file isn't EXT2 return NULL
  if (sb.s_magic != EXT2_SUPER_MAGIC) {
    free(vol);
    return NULL;
  }
  //set block size, volume size and number of groups
  vol->block_size = 1024 << sb.s_log_block_size;
  vol->volume_size = sb.s_blocks_count * vol->block_size;
  if (sb.s_blocks_count % sb.s_blocks_per_group == 0) {
    vol->num_groups = sb.s_blocks_count / sb.s_blocks_per_group;
  } else {
    vol->num_groups = sb.s_blocks_count / sb.s_blocks_per_group + 1;
  }

  //set the groups array up and give it enough memory for the whole table
  vol->groups = malloc(vol->num_groups * sizeof(group_desc_t));
  int offset = 0;

  if (vol->block_size <= EXT2_OFFSET_SUPERBLOCK) {
    //block table starts in the third block
    offset = 2 * vol->block_size;
  } else {
    //block table starts in the second block
    offset = vol->block_size;
  }

  //move fd pointer to correct place to start reading block descriptor table
  lseek(vol->fd, offset, SEEK_SET);

  //fill the group table 
  for (uint32_t i = 0 ; i < vol->num_groups; i++) {
    read(vol->fd, &vol->groups[i], sizeof(group_desc_t));
  }

  //set file pointer back to the start of the volume in order to do future reads
  lseek(vol->fd, 0, SEEK_SET);

  vol->super = sb;
  return vol;
}

/* close_volume_file: Frees and closes all resources used by a EXT2 volume.
   
   Parameters:
     volume: pointer to volume to be freed.
 */
void close_volume_file(volume_t *volume) {
  //need to free memory (groups and volume pointer)
  //need to close file pointer
    close(volume->fd);
    free(volume->groups);
    volume->groups = NULL;
    free(volume);
    volume = NULL;
}

/* read_block: Reads data from one or more blocks. Saves the resulting
   data in buffer 'buffer'. This function also supports sparse data,
   where a block number equal to 0 sets the value of the corresponding
   buffer to all zeros without reading a block from the volume.
   
   Parameters:
     volume: pointer to volume.
     block_no: Block number where start of data is located.
     offset: Offset from beginning of the block to start reading
             from. May be larger than a block size.
     size: Number of bytes to read. May be larger than a block size.
     buffer: Pointer to location where data is to be stored.

   Returns:
     In case of success, returns the number of bytes read from the
     disk. In case of error, returns -1.
 */
ssize_t read_block(volume_t *volume, uint32_t block_no, uint32_t offset, uint32_t size, void *buffer) {

  //if block num is invalid, error
  if (block_no < 0 || block_no > volume->super.s_blocks_count) {
    return EXT2_INVALID_BLOCK_NUMBER;
  } else if (block_no == 0) { //if block number is 0, buffer is set to 0 and return without reading
    memset(buffer, 0, size);
    return size;
  }

  return pread(volume->fd, buffer, size, block_no * volume->block_size + offset);
}

/* read_inode: Fills an inode data structure with the data from one
   inode in disk. Determines the block group number and index within
   the group from the inode number, then reads the inode from the
   inode table in the corresponding group. Saves the inode data in
   buffer 'buffer'.
   
   Parameters:
     volume: pointer to volume.
     inode_no: Number of the inode to read from disk.
     buffer: Pointer to location where data is to be stored.

   Returns:
     In case of success, returns a positive value. In case of error,
     returns -1.
 */
ssize_t read_inode(volume_t *volume, uint32_t inode_no, inode_t *buffer) {

  if (inode_no == 0) {
    return EXT2_INVALID_BLOCK_NUMBER;
  }
  
  //calculations to find the correct inode within the volume
  uint32_t offset = (inode_no - 1) % volume->super.s_inodes_per_group;
  uint32_t group = (inode_no - 1) / volume->super.s_inodes_per_group;
  uint32_t itable = volume->groups[group].bg_inode_table;
  offset *= volume->super.s_inode_size;

  return read_block(volume, itable, offset, volume->super.s_inode_size, buffer);
}

/* read_ind_block_entry: Reads one entry from an indirect
   block. Returns the block number found in the corresponding entry.
   
   Parameters:
     volume: pointer to volume.
     ind_block_no: Block number for indirect block.
     index: Index of the entry to read from indirect block.

   Returns:
     In case of success, returns the block number found at the
     corresponding entry. In case of error, returns
     EXT2_INVALID_BLOCK_NUMBER.
 */
static uint32_t read_ind_block_entry(volume_t *volume, uint32_t ind_block_no,
				     uint32_t index) {
              
    if (ind_block_no < 0) {
      return EXT2_INVALID_BLOCK_NUMBER;
    }

    uint32_t blockNum;
    //want to read the block number and need to give offset in bytes, thus multiply by size of pointer
    ssize_t num = read_block(volume, ind_block_no, index * sizeof(uint32_t), sizeof(uint32_t), &blockNum);
    if (num != EXT2_INVALID_BLOCK_NUMBER) {
      return blockNum;
    } else {
      return EXT2_INVALID_BLOCK_NUMBER;
    }
}

/* read_inode_block_no: Returns the block number containing the data
   associated to a particular index. For indices 0-11, returns the
   direct block number; for larger indices, returns the block number
   at the corresponding indirect block.
   
   Parameters:
     volume: Pointer to volume.
     inode: Pointer to inode structure where data is to be sourced.
     index: Index to the block number to be searched.

   Returns:
     In case of success, returns the block number to be used for the
     corresponding entry. This block number may be 0 (zero) in case of
     sparse files. In case of error, returns
     EXT2_INVALID_BLOCK_NUMBER.
 */
static uint32_t get_inode_block_no(volume_t *volume, inode_t *inode, uint64_t block_idx) {
  //block number must be 0 or larger
  if (block_idx < 0) {
    return EXT2_INVALID_BLOCK_NUMBER;
  }

  //if block idx is within first 12, then it is in a direct block
  if (block_idx < 12) {
    return inode->i_block[block_idx];
  } 
  
  //number of blocks in indirect block is blocksize/pointersize
  uint64_t numBlocks = volume->block_size / sizeof(uint32_t);
  //shift block index such that is aligned with indirect block indexing
  block_idx -= 12;
  if (block_idx < numBlocks) {
    return read_ind_block_entry(volume, inode->i_block_1ind, block_idx);
  }

  //shift block index so it is aligned with second indirect block indexing
  block_idx -= numBlocks;
  if (block_idx < numBlocks * numBlocks) {
    uint32_t firstInd = read_ind_block_entry(volume, inode->i_block_2ind, (block_idx / numBlocks));
    return read_ind_block_entry(volume, firstInd, (block_idx % numBlocks)); 
  }

  //shift block index so it is aligned with third indirect block indexing
  block_idx -= (numBlocks * numBlocks);
  if (block_idx < (numBlocks * numBlocks * numBlocks)) {
    //set the index to the third indirect block
    uint64_t index = block_idx / (numBlocks * numBlocks);
    uint32_t secondInd = read_ind_block_entry(volume, inode->i_block_3ind, index);
    //set the index to use with second indirect block
    index = (block_idx / numBlocks) % numBlocks;
    uint32_t firstInd = read_ind_block_entry(volume, secondInd, index);
    //set the index to use with the indirect block
    index = block_idx % numBlocks;
    return read_ind_block_entry(volume, firstInd, index);
  }

  return EXT2_INVALID_BLOCK_NUMBER;
}

/* read_file_block: Returns the content of a specific file, limited to
   a single block.
   
   Parameters:
     volume: Pointer to volume.
     inode: Pointer to inode structure for the file.
     offset: Offset, in bytes from the start of the file, of the data
             to be read.
     max_size: Maximum number of bytes to read from the block.
     buffer: Pointer to location where data is to be stored.

   Returns:
     In case of success, returns the number of bytes read from the
     disk. In case of error, returns -1.
 */
ssize_t read_file_block(volume_t *volume, inode_t *inode, uint64_t offset, uint64_t max_size, void *buffer) {
  //set up the block index and block offset in order to find inode number and read from volume correctly
  uint64_t blockIdx = offset / volume->block_size; 
  uint64_t blockOffset = offset % volume->block_size; 
  uint32_t blockNum = get_inode_block_no(volume, inode, blockIdx);
  
  //if we get an error from get_inode_block_no, return error
  if (blockNum == EXT2_INVALID_BLOCK_NUMBER) {
    return EXT2_INVALID_BLOCK_NUMBER;
  }

  //if we want to read past the end of the file, need to change size so we don't
  if (max_size + offset > inode_file_size(volume, inode)) {
    max_size = inode_file_size(volume, inode) - offset;
  }
  
  //use block offset instead of byte offset
  if (max_size + blockOffset > volume->block_size) {
    max_size = volume->block_size - blockOffset;
  }

  return read_block(volume, blockNum, blockOffset, max_size, buffer);
}

/* read_file_content: Returns the content of a specific file, limited
   to the size of the file only. May need to read more than one block,
   with data not necessarily stored in contiguous blocks.
   
   Parameters:
     volume: Pointer to volume.
     inode: Pointer to inode structure for the file.
     offset: Offset, in bytes from the start of the file, of the data
             to be read.
     max_size: Maximum number of bytes to read from the file.
     buffer: Pointer to location where data is to be stored.

   Returns:
     In case of success, returns the number of bytes read from the
     disk. In case of error, returns -1.
 */
ssize_t read_file_content(volume_t *volume, inode_t *inode, uint64_t offset, uint64_t max_size, void *buffer) {

  uint32_t read_so_far = 0;

  if (offset + max_size > inode_file_size(volume, inode))
    max_size = inode_file_size(volume, inode) - offset;
  
  while (read_so_far < max_size) {
    int rv = read_file_block(volume, inode, offset + read_so_far,
			     max_size - read_so_far, buffer + read_so_far);
    if (rv <= 0) return rv;
    read_so_far += rv;
  }
  return read_so_far;
}

/* follow_directory_entries: Reads all entries in a directory, calling
   function 'f' for each entry in the directory. Stops when the
   function returns a non-zero value, or when all entries have been
   traversed.
   
   Parameters:
     volume: Pointer to volume.
     inode: Pointer to inode structure for the directory.
     context: This pointer is passed as an argument to function 'f'
              unmodified.
     buffer: If function 'f' returns non-zero for any file, and this
             pointer is set to a non-NULL value, this buffer is set to
             the directory entry for which the function returned a
             non-zero value. If the pointer is NULL, nothing is
             saved. If none of the existing entries returns non-zero
             for 'f', the value of this buffer is unspecified.
     f: Function to be called for each directory entry. Receives three
        arguments: the file name as a NULL-terminated string, the
        inode number, and the context argument above.

   Returns:
     If the function 'f' returns non-zero for any directory entry,
     returns the inode number for the corresponding entry. If the
     function returns zero for all entries, or the inode is not a
     directory, or there is an error reading the directory data,
     returns 0 (zero).
 */
uint32_t follow_directory_entries(volume_t *volume, inode_t *inode, void *context,
				  dir_entry_t *buffer,
				  int (*f)(const char *name, uint32_t inode_no, void *context)) {
  
  //inode passed isn't a directory
  if ((inode->i_mode & S_IFDIR) != S_IFDIR) {
    return 0;
  }

  dir_entry_t directory;
  //begin looping through file entries, want to ensure offset less than file size
  for (uint32_t offset = 0; offset < inode_file_size(volume, inode); offset += directory.de_rec_len) {
    ssize_t readSize = read_file_block(volume, inode, offset, sizeof(dir_entry_t), &directory);
    if (readSize < 0) {
      return 0;
    }
    
    //if inode number is invalid, return 0
    if (directory.de_inode_no <= 0) {
      return 0;
    }
    //need a null-terminated string
    char name[directory.de_name_len + 1];
    strncpy(name, directory.de_name, directory.de_name_len);
    name[directory.de_name_len] = '\0';
    
    //call f on every directory entry
    int retVal = f(name, directory.de_inode_no, context);

    //if non zero, return the inode num and assign buffer appropriately
    if (retVal != 0) {
      uint32_t inodeNum = directory.de_inode_no;
      if (buffer != NULL) {
        *buffer = directory;
      }
      return inodeNum;
    }
  }

  return 0;
}

/* Simple comparing function to be used as argument in find_file_in_directory function */
static int compare_file_name(const char *name, uint32_t inode_no, void *context) {
  return !strcmp(name, (char *) context);
}

/* find_file_in_directory: Searches for a file in a directory.
   
   Parameters:
     volume: Pointer to volume.
     inode: Pointer to inode structure for the directory.
     name: NULL-terminated string for the name of the file. The file
           name must match this name exactly, including case.
     buffer: If the file is found, and this pointer is set to a
             non-NULL value, this buffer is set to the directory entry
             of the file. If the pointer is NULL, nothing is saved. If
             the file is not found, the value of this buffer is
             unspecified.

   Returns:
     If the file exists in the directory, returns the inode number
     associated to the file. If the file does not exist, or the inode
     is not a directory, or there is an error reading the directory
     data, returns 0 (zero).
 */
uint32_t find_file_in_directory(volume_t *volume, inode_t *inode, const char *name, dir_entry_t *buffer) {
  
  return follow_directory_entries(volume, inode, (char *) name, buffer, compare_file_name);
}

/* find_file_from_path: Searches for a file based on its full path.
   
   Parameters:
     volume: Pointer to volume.
     path: NULL-terminated string for the full absolute path of the
           file. Must start with '/' character. Path components
           (subdirectories) must be delimited by '/'. The root
           directory can be obtained with the string "/".
     dest_inode: If the file is found, and this pointer is set to a
                 non-NULL value, this buffer is set to the inode of
                 the file. If the pointer is NULL, nothing is
                 saved. If the file is not found, the value of this
                 buffer is unspecified.

   Returns:
     If the file exists, returns the inode number associated to the
     file. If the file does not exist, or there is an error reading
     any directory or inode in the path, returns 0 (zero).
 */
uint32_t find_file_from_path(volume_t *volume, const char *path, inode_t *dest_inode) {

  //path doesn't start with Root Directory, return 0
  if (path[0] != '/') {
    return 0;
  }
  //gives us the root inode
  inode_t inode;
  uint32_t inodeNum = EXT2_ROOT_INO;
  if (read_inode(volume, inodeNum, &inode) < 0) {
    return 0;
  }

  //duplicate name since path is declared const
 uint8_t length = strlen(path);
  char newPath[length+1];
  strncpy(newPath, path, length);
  newPath[length] = '\0';
  //gets the name of the directory, delimited by /
  char *name = strtok(newPath, "/");
  dir_entry_t dirEntry;

  //while name is not NULL, continuing moving through path
  while(name) {
    //find the file in the directory entry
    inodeNum = find_file_in_directory(volume, &inode, name, &dirEntry);

    //if inodeNum <= 0, didn't find the file and return 0
    if (inodeNum <= 0) {
      return 0;
    }
    //set the next inode number to the directory inode so that we can read its data
    inodeNum = dirEntry.de_inode_no;
    //if we cannot read the inode, return 0
    if (read_inode(volume, inodeNum, &inode) <= 0) {
      return 0;
    }

    //set name to next name in the directory
    name = strtok(NULL, "/");
  }

  //if the dest_inode is not null, set it to the inode's information
  if (dest_inode != NULL) {
    *dest_inode = inode;
  }
  
  return inodeNum;
}
