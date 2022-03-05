#include "sfs_api.h"
#include "disk_emu.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define BLOCK_SIZE 512
#define MAX_BLOCKS 500

#define DISK_FILE "sfs_disk.disk"
#define MAX_INODES 50 
#define MAX_FILES 50
#define MAX_FILE_SIZE (BLOCK_SIZE * 12) + (BLOCK_SIZE * (BLOCK_SIZE / sizeof(unsigned int))) //number of pointer available  

void zeroCache(); //zeroes contents in RAM
void init_sb(); //init superblock
void add_root_dir_inode(); 
void init_free_blocks(); 
int findInDir(const char *path); //finds path in the directory
int findInFd (unsigned int inodeNumber); //finds if present in FD table
void findOpenInode(int number);//finds lowest empty inode space
void findOpenDir(int number);//finds lowest empty Direcotry space
void findOpenFd(int number);//finds lowest empty file descriptor space
void findCurrentBlock(int number);

super_block_t sb; //the super block
dir_entry_t root_dir[MAX_FILES]; //array of directory entris will be the directory structure
inode_t inode_table[MAX_INODES]; //the inode table will be array of inodes
fd_table_entry_t fd_table[MAX_FILES]; //the file descriptor table
unsigned int free_blocks[MAX_BLOCKS]; //free blocks jsut an int array with each index mapped to corresponding block
unsigned int currentBlock; //the earliest empty block
unsigned int availableInode; //will represent the lowest value inode space available
unsigned int availableDirectory; //lowest value of empty dir index
unsigned int availableFd = 0; //lowest empty space in fd table
unsigned int num_inode_blocks; //the length fo the inode table in terms of blocks
unsigned int dirStartBlock;//the starting block of the directory
unsigned int freeBlocksLength; //length of freeblocks in terms of blocks
unsigned int nextFile = 0; //used in get next file name

void mksfs(int fresh) {
	
	void *blockContent, *inodeContent, *rootDirContent, *freeBlocksContent;
	unsigned int num_dir_blocks;

	if(fresh == 1){
		init_fresh_disk(DISK_FILE, BLOCK_SIZE, MAX_BLOCKS);//initializes the disk of given name and of the size
		zeroCache(); 

		//create superblock and write it to disk
		init_sb();
		write_blocks(0, 1, &sb); //write to block 0 the contents of sb	
		currentBlock = 1;

		//initialize inode table
		add_root_dir_inode(); //create root inode
		num_inode_blocks = (sizeof(inode_t) * MAX_INODES) / BLOCK_SIZE + 1; //take down its length
		write_blocks(currentBlock, num_inode_blocks, &inode_table); 
		availableInode = 1; //one inode used
		currentBlock += num_inode_blocks; //adjust what block is free

		//create and write root dir
		dirStartBlock = currentBlock;//keep track of its start block
		write_blocks(currentBlock, inode_table[0].size, &root_dir);//write at the block with the length
		availableDirectory = 0; 
		currentBlock += inode_table[0].size; //adjust current block

		//init the free blocks array and write it
		init_free_blocks();
		freeBlocksLength = sizeof(free_blocks) / BLOCK_SIZE + 1;
		write_blocks(MAX_BLOCKS - freeBlocksLength, freeBlocksLength, &free_blocks);

		//reset other variables
		availableFd = 0; 
		bzero(fd_table, sizeof(fd_table));
	}
	else {
		init_disk(DISK_FILE, BLOCK_SIZE, MAX_BLOCKS);

		num_inode_blocks = (sizeof(inode_t) * MAX_INODES) / BLOCK_SIZE + 1; //need how many blocks inode table is
		num_dir_blocks = (sizeof(dir_entry_t) * MAX_FILES) / BLOCK_SIZE + 1; //need to know how many blocks directory is
		freeBlocksLength = sizeof(free_blocks) / BLOCK_SIZE + 1;


		blockContent = (void*) calloc(1, BLOCK_SIZE); //create buffer to work with
		inodeContent = (void*) calloc(1, BLOCK_SIZE * num_inode_blocks);
		rootDirContent = (void*) calloc(1, BLOCK_SIZE * num_dir_blocks); 
		freeBlocksContent = (void*) calloc(1, BLOCK_SIZE * freeBlocksLength);

		//read super block and place in sb
		read_blocks(0, 1, blockContent); 
		memcpy(&sb, blockContent, sizeof(sb));//copy proper size

		//read inode table
		read_blocks(1, num_inode_blocks, inodeContent); 
		memcpy(inode_table, inodeContent, sizeof(inode_table)); //copy

		//read directory
		dirStartBlock = num_inode_blocks + 1; 
		read_blocks(dirStartBlock, num_dir_blocks, rootDirContent);
		memcpy(root_dir, rootDirContent, sizeof(root_dir));

		//readfree_blocks array
		read_blocks(MAX_BLOCKS - freeBlocksLength, freeBlocksLength, freeBlocksContent);
		memcpy(free_blocks, freeBlocksContent, sizeof(free_blocks));

		//now get other helper variables
		findCurrentBlock(dirStartBlock + num_dir_blocks - 1);
		findOpenInode(1);
		findOpenDir(0);
		
		//need to zero file descriptor table
		availableFd = 0; 
		bzero(fd_table, sizeof(fd_table));

		nextFile = 0;

		free(blockContent);
		free(inodeContent);
		free(rootDirContent);
		free(freeBlocksContent);
	}
	return;
}

//retrieves next file name in the directory
int sfs_getnextfilename(char *fname) {
	char *zero = (char*) calloc(1, sizeof(char) * 20); //empty buffer to check if empty directory location
	
	while(nextFile < MAX_FILES && (strlen(root_dir[nextFile].file_name) == 0 || memcmp(zero, root_dir[nextFile].file_name, 20) == 0)){
		nextFile++;//if empty directory location increment
	}

	//if too large though reset nextFile and return 0
	if(nextFile >= MAX_FILES){
		nextFile = 0; //reset it
		free(zero);
		return 0;
	}	
	
	//at this point can copy the dir entry into fname
	strcpy(fname, root_dir[nextFile].file_name);
	nextFile++;
	free(zero);
	return 1;
}

//gets the size of the given file
int sfs_getfilesize(const char* path) {
	unsigned int inodeNumber;	
	int index;
	index = findInDir(path);
	inodeNumber = root_dir[index].inode;
	if(index >= 0)
		return inode_table[inodeNumber].size;
	else{
		printf("File not found\n");
		return -1;
	}	
	
}

//opens the given file
int sfs_fopen(char *name) {	
	unsigned int fd, i, goodExt;
	char *ext; 
	int index;
	
	//find its index in the directory
	index = findInDir(name);

	if(index == -1){
	
		//first check if space in inode table
		if(availableInode >= MAX_INODES || currentBlock >= MAX_BLOCKS || availableDirectory >= MAX_INODES || availableFd >= MAX_FILES){
			printf("There is no space available for another file\n");
			return -1;
		}

		if(strlen(name) == 0){
			printf("Need filename longer than length 0\n");
			return -1;
		}

	
		inode_table[availableInode].mode = 0x755;
		inode_table[availableInode].link_cnt = 1;
		inode_table[availableInode].uid = 0;
		inode_table[availableInode].gid = 0;
		inode_table[availableInode].size = 0;
		for(i = 0; i < 12; i++){
			inode_table[availableInode].data_ptrs[i] = 0; 
		}

		write_blocks(1, num_inode_blocks, &inode_table); //write the updated inode table to disk

		//deals with extension or not
		ext = strchr(name, '.');
		if((ext != NULL && strlen(ext) <= 4) || ext == NULL){
			goodExt = 1;
		}
		else{
			goodExt = 0;
		}

		//create new directory entry
		if(strlen(name) > 20 || !goodExt){ 
			printf("File name is invalid. Must not be longer than 20 char total and extension cannot be longer than 3 char\n");
			return -1;
		}
		root_dir[availableDirectory].file_name[strlen(name)] = '\0'; //add mull terminator
		strcpy(root_dir[availableDirectory].file_name, name); 
		root_dir[availableDirectory].inode = availableInode;	

		write_blocks(dirStartBlock, inode_table[0].size, &root_dir); 
		index = availableDirectory; 
		
		findOpenInode(availableInode);
		findOpenDir(availableDirectory); 
	}	


	if(findInFd(root_dir[index].inode) != -1){
		printf("File already open\n");
		return -1;
	}

	
	//if no space in fd table
	if(availableFd >= MAX_FILES){
		printf("No more space available\n");
		return -1;
	}
	
	fd_table[availableFd].inode_number = root_dir[index].inode; //in fd table set the inode number
	fd_table[availableFd].rw_pointer = inode_table[root_dir[index].inode].size; //set read write pointer to end of file
	

	fd = availableFd; 
	findOpenFd(availableFd);//update availe fd index

	return fd;//return file descriptor for the file opened
}

//closes the given file
int sfs_fclose(int fileID){	
	
	if(fileID < 0 || fileID >= MAX_INODES){
		printf("Bad fileID\n");
		return -1;
	}
	//first check if present in table
	if(fd_table[fileID].inode_number == 0){
		printf("There is no file with give handle\n");
		return -1;
	}

	
	fd_table[fileID].inode_number = 0;
	fd_table[fileID].rw_pointer = 0;

	
	if(fileID < availableFd)
		availableFd = fileID;

	return 0;
}

//reads the given fiel into buffer
int sfs_fread(int fileID, char *buf, int length){
	//find reference within the file descriptor table and get the inode
	char *blockContent;	
	unsigned int *indirectBlock, workingBlock, block_number, inodeNumber, loc_in_block, bytesRead = 0;

	
	blockContent = (char*) calloc(1, BLOCK_SIZE); 
	indirectBlock = (unsigned int*) calloc(1, BLOCK_SIZE);

	inodeNumber = fd_table[fileID].inode_number;
	
	
	if(inodeNumber == 0){
		printf("File descriptor does not exist\n");
		return 0; //return no bytes read
	}
	
	
	if(inode_table[inodeNumber].size == 0){
		strcpy(buf, "");
		return 0;
	}

	
	if(length <= 0){
		printf("Cannot read 0 or less\n");
		return 0;
	}

	//if length too long just cut it down to proper size
	if(fd_table[fileID].rw_pointer + length > inode_table[inodeNumber].size){
		length = inode_table[inodeNumber].size - fd_table[fileID].rw_pointer;
	}

	//read the data. need loop in the event that read spans multiple blocks
	while(length > 0){
		
		block_number = fd_table[fileID].rw_pointer / BLOCK_SIZE; 
		loc_in_block = fd_table[fileID].rw_pointer % BLOCK_SIZE;

		//if on an indirect pointer
		if(block_number > 11){
			
			read_blocks(inode_table[inodeNumber].indirect_pointer, 1, indirectBlock);
			workingBlock = indirectBlock[block_number - 12];
		}
		//if using a direct pointer
		else{
			workingBlock = inode_table[inodeNumber].data_ptrs[block_number];
		}

		//read the block into a buffer
		read_blocks(workingBlock, 1, blockContent);
			
		//if rest of read will be contained within current block
		if(length + loc_in_block < BLOCK_SIZE){
			memcpy(&buf[bytesRead], &blockContent[loc_in_block], length); 
			bytesRead += length; 
			fd_table[fileID].rw_pointer += length; 
			length -= length; 

		}

		//if read will have to span to next block
		else{
			memcpy(&buf[bytesRead], &blockContent[loc_in_block], BLOCK_SIZE - loc_in_block);
			bytesRead += BLOCK_SIZE - loc_in_block; 
			fd_table[fileID].rw_pointer += BLOCK_SIZE -loc_in_block; 
			length -= BLOCK_SIZE - loc_in_block; 
		}
	}


	free(blockContent);//free the buffer created
	free(indirectBlock);

	return bytesRead; 
}

//writes the given file into buf
int sfs_fwrite(int fileID, const char *buf, int length){
	char *blockContent;	
	unsigned int *indirectBlock, currentIndirectPointer, inodeNumber, block_number, workingBlock, loc_in_block, i, bytesWritten = 0;

	//make buffers
	blockContent = (char*) calloc(1, BLOCK_SIZE); 
	indirectBlock = (unsigned int*) calloc(1, BLOCK_SIZE);

	//get inode number
	inodeNumber = fd_table[fileID].inode_number;

	//if bad handle
	if(inodeNumber == 0){
		printf("File descriptor does not exist\n");
		return 0;
	}

	//if file is too large
	if(length + fd_table[fileID].rw_pointer > MAX_FILE_SIZE){
		printf("The file size is too large. Will cut off and only write %lu\n", MAX_FILE_SIZE);
		length = MAX_FILE_SIZE - fd_table[fileID].rw_pointer;
	}

	//if 0 length
	if(length <= 0){
		printf("Cannot write 0 or less\n");
		return 0;
	}

	//if the file is empty, need to allot it a block
	if(inode_table[inodeNumber].size == 0){

		if(currentBlock >= MAX_BLOCKS){
			printf("No space available\n");
			return 0;
		}

		inode_table[inodeNumber].data_ptrs[0] = currentBlock;
		free_blocks[currentBlock] = 1;
		findCurrentBlock(currentBlock);
	}

	while(length > 0){
		//get the block that the r/w pointer is on and where within the block it is
		block_number = fd_table[fileID].rw_pointer / BLOCK_SIZE; 
		loc_in_block = fd_table[fileID].rw_pointer % BLOCK_SIZE;

		if(block_number < 12 && inode_table[inodeNumber].data_ptrs[block_number] == 0){
			//if no space
			if(currentBlock >= MAX_BLOCKS){
				printf("No more space available to complete write of file\n");
				break;
			}
			else{
				inode_table[inodeNumber].data_ptrs[block_number] = currentBlock; 
				free_blocks[currentBlock] = 1; 
				findCurrentBlock(currentBlock); 
			}
		}
		//if first use fo indirect pointer
		else if(block_number == 12 && inode_table[inodeNumber].indirect_pointer == 0){
			//need to give it a block
			//if no space
			if(currentBlock >= MAX_BLOCKS){
				printf("No more space available to complete write of file\n");
				break;
			}
			else{
				inode_table[inodeNumber].indirect_pointer = currentBlock; //set the indirect pointer
				free_blocks[currentBlock] = 1; 
				findCurrentBlock(currentBlock);
				indirectBlock[0] = currentBlock; 
				currentIndirectPointer = currentBlock; 
				write_blocks(inode_table[inodeNumber].indirect_pointer, 1, indirectBlock); //write into the indeirect pointer the new block
				free_blocks[currentBlock] = 1;//set in free blocks array
				findCurrentBlock(currentBlock);
			}
		}

		else if(block_number > 12 && inode_table[inodeNumber].indirect_pointer != 0){ 
			read_blocks(inode_table[inodeNumber].indirect_pointer, 1, indirectBlock); //read the contents fo the block

			 
			if(indirectBlock[block_number - 12] != 0){
				currentIndirectPointer = indirectBlock[block_number - 12]; 
			}
			else{
				//if no spaceon disk
				if(currentBlock >= MAX_BLOCKS){
					printf("No more space available to complete write of file\n");
					break;
				}
				//if no more pointers available
				else if(block_number > 12 + (BLOCK_SIZE / sizeof(unsigned int))){
					printf("No more pointers avaialble to the file. You have reached max file size\n");
					break;
				}

				read_blocks(inode_table[inodeNumber].indirect_pointer, 1, indirectBlock);//read the block

				//find next empty pointer in the block
				for(i=0; i < BLOCK_SIZE / sizeof(unsigned int); i++){
					if(indirectBlock[i] == 0){
						indirectBlock[i] = currentBlock; 
						currentIndirectPointer = currentBlock; 
						write_blocks(inode_table[inodeNumber].indirect_pointer, 1, indirectBlock); 
						free_blocks[currentBlock] = 1; 
						findCurrentBlock(currentBlock);
						break;
					}
				}
			}
		}


		if(block_number > 11){
			read_blocks(currentIndirectPointer, 1, blockContent); //read the block
			workingBlock = currentIndirectPointer;
		}
		//if direct pointer
		else{
			//read the block into buffer
			read_blocks(inode_table[inodeNumber].data_ptrs[block_number], 1, blockContent);
			workingBlock = inode_table[inodeNumber].data_ptrs[block_number];
		}


		//if write will finish within this block
		if(length + loc_in_block < BLOCK_SIZE){
			memcpy(&blockContent[loc_in_block], &buf[bytesWritten], length);
			write_blocks(workingBlock, 1, blockContent); //write it
			bytesWritten += length; 
			fd_table[fileID].rw_pointer += length;
			length -= length;
			
		}

		else{
			memcpy(&blockContent[loc_in_block], &buf[bytesWritten], BLOCK_SIZE - loc_in_block);//copy until the end of the block
			write_blocks(workingBlock, 1, blockContent);
			bytesWritten += BLOCK_SIZE - loc_in_block; 
			fd_table[fileID].rw_pointer += BLOCK_SIZE - loc_in_block; 
			length -= BLOCK_SIZE -loc_in_block; 
			
		}
			
	}

	//adjust size if needed
	if(fd_table[fileID].rw_pointer > inode_table[inodeNumber].size){
		inode_table[inodeNumber].size = fd_table[fileID].rw_pointer;
	}
	
	write_blocks(1, num_inode_blocks, &inode_table); 
	write_blocks(MAX_BLOCKS - freeBlocksLength, freeBlocksLength, &free_blocks);
	

	//free the buffers created
	free(blockContent);
	free(indirectBlock);

	return bytesWritten;
}

// seek to the location from beginning
int sfs_fseek(int fileID, int loc){
	unsigned int inodeNumber;

	inodeNumber = fd_table[fileID].inode_number;  

	//if provided locaiton to large
	if(loc > inode_table[inodeNumber].size){
		fd_table[fileID].rw_pointer = inode_table[inodeNumber].size;
	}
	//if too small
	else if(loc < 0){
		fd_table[fileID].rw_pointer = 0;
	}
	else{
		fd_table[fileID].rw_pointer = loc;
	}

	return 0;
}

//removes given file from file system
int sfs_remove(char *file) {
	
	unsigned int inodeNumber, *zeroBlock, i, *indirectBlock;
	int dirIndex, fd;

	zeroBlock = (unsigned int*) calloc(1, BLOCK_SIZE); 
	indirectBlock = (unsigned int*) calloc(1, BLOCK_SIZE); 

	dirIndex = findInDir(file);
	if(dirIndex == -1){
		printf("File does not exist\n");
		return 1;
	}

	//get inode number
	inodeNumber = root_dir[dirIndex].inode;

	//check if open. if it is remove it
	fd = findInFd(inodeNumber);
	if(fd != -1){
		fd_table[fd].rw_pointer = 0;
		fd_table[fd].inode_number = 0;
		if(fd < availableFd){
			availableFd = fd; //and reset availablefd if the one deleted has lower value
		}	
	}

	//remove the direct data blocks
	for(i = 0; i < 12; i++){
		write_blocks(inode_table[inodeNumber].data_ptrs[i], 1, zeroBlock);
		if(inode_table[inodeNumber].data_ptrs[i] != 0){
			free_blocks[inode_table[inodeNumber].data_ptrs[i]] = 0; 
		}
		inode_table[inodeNumber].data_ptrs[i] = 0; 
	}

	//then remove the blocks pointed to by indirect pointer block
	read_blocks(inode_table[inodeNumber].indirect_pointer, 1, indirectBlock); //read the set of pointers
	for(i = 0; i < BLOCK_SIZE / sizeof(unsigned int); i++){
		if(indirectBlock[i] != 0){
			write_blocks(indirectBlock[i], 1, zeroBlock); 
			free_blocks[indirectBlock[i]] = 0; 
		}
		else{
			break;
		}
	}

	//then take care of the block holding those pointers
	write_blocks(inode_table[inodeNumber].indirect_pointer, 1, zeroBlock);
	if(inode_table[inodeNumber].indirect_pointer != 0){
		free_blocks[inode_table[inodeNumber].indirect_pointer] = 0; 
	}
	inode_table[inodeNumber].indirect_pointer = 0; 

	findCurrentBlock(dirStartBlock + inode_table[0].size - 1);

	//remove the inode 
	inode_table[inodeNumber].mode = 0;
	inode_table[inodeNumber].link_cnt = 0;
	inode_table[inodeNumber].uid = 0;
	inode_table[inodeNumber].gid = 0;
	inode_table[inodeNumber].size = 0;

	//reset availableInode if inode has lower value
	if(inodeNumber < availableInode){
		availableInode = inodeNumber;
	}

	//remove the directory entry
	//root_dir[dirIndex].file_name = "\0";
	bzero(root_dir[dirIndex].file_name, 20);
	root_dir[dirIndex].inode = 0;

	//reset direntr if needed
	if(dirIndex <availableDirectory){
		availableDirectory = dirIndex;
	}
	
	write_blocks(1, num_inode_blocks, inode_table);
	write_blocks(dirStartBlock, inode_table[0].size, root_dir);
	write_blocks(MAX_BLOCKS - freeBlocksLength, freeBlocksLength, free_blocks);
	

	//free the buffer we created
	free(zeroBlock);
	free(indirectBlock);

	return 0;
}


void init_sb(){
	sb.magic = 1234;
	sb.block_size = BLOCK_SIZE;
	sb.fs_size = MAX_BLOCKS * BLOCK_SIZE;
	sb.inode_table_length = MAX_INODES;
	sb.root_dir_inode = 0; //why is this 0
}

void zeroCache(){
	bzero(&sb, sizeof(super_block_t)); 
	bzero(inode_table, sizeof(inode_table)); 
	bzero(root_dir, sizeof(root_dir)); 
	bzero(free_blocks, sizeof(free_blocks)); 
	bzero(fd_table, sizeof(fd_table));
}

void add_root_dir_inode(){
	inode_table[0].mode = 0x755; 
	inode_table[0].link_cnt = 1;
	inode_table[0].uid = 0; 
	inode_table[0].gid = 0; 
	inode_table[0].size = (sizeof(dir_entry_t) * MAX_FILES) / BLOCK_SIZE + 1; 
	inode_table[0].data_ptrs[0] = num_inode_blocks + 1; //store in the blcok after the superblock and inode table
}

void init_free_blocks(){
	
	int i;
	for(i = 0; i < currentBlock; i++){
		free_blocks[i] = 1; 
	}

	for(i = MAX_BLOCKS - freeBlocksLength; i < MAX_BLOCKS; i ++){
		free_blocks[i] = 1;
	}
}

int findInDir(const char *path){

	unsigned int i, isPresent = 0;
	//loop through until find it
	for(i = 0; i < MAX_INODES; i++){
		if(strcmp(root_dir[i].file_name, path) == 0){
			isPresent = 1;
			break;
		}
	}

	if(isPresent)
		return i;
	else
		return -1;
}


int findInFd(unsigned int inodeNumber){
	unsigned int i;
	//search through fd table if find number
	for(i = 0; i < MAX_FILES; i++){
		if(fd_table[i].inode_number == inodeNumber){
			return i;
		}
	}
	return -1;
}

	
void findOpenInode(int number){
	unsigned int i = number;
	for(i = number; i < MAX_INODES; i++){
		
		if(inode_table[i].link_cnt == 0){ 
			availableInode = i; 
			return;
		}
	}
	
	
	availableInode = MAX_INODES; 
}

void findOpenDir(int number){
	unsigned int i = number;
	for(i = number; i < MAX_INODES; i++){
		if(root_dir[i].file_name[0] == '\0'){
			availableDirectory = i;
			return;
		}
	}
	
	//out of space
	availableDirectory = MAX_INODES; //have conditions that will check this	
}

void findOpenFd(int number){
	unsigned int i = number;
	for(i = number; i < MAX_FILES; i++){
		if(fd_table[i].inode_number == 0){
			availableFd = i;
			return;
		}
	}
	
	availableDirectory = MAX_FILES; 
}	

void findCurrentBlock(int number){
	unsigned int i;
	for(i = number; i < MAX_BLOCKS; i++){
		//search until find free block
		if(free_blocks[i] == 0){
			currentBlock = i; 
			return;
		}
	}

	currentBlock = MAX_BLOCKS; 

			currentBlock = i;
}

