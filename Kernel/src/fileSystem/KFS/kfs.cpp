#include "kfs.h"

namespace FileSystem{

    /* KFS */
    KFS::KFS(GPT::Partition* partition){
        globalPartition = partition;
        KFSPartitionInfo = (KFSinfo*)mallocK(sizeof(KFSinfo));
        while(true){
            globalPartition->Read(0, sizeof(KFSinfo), KFSPartitionInfo);
            InitKFS();
            if(KFSPartitionInfo->IsInit.Data1 == GUIDData1 && KFSPartitionInfo->IsInit.Data2 == GUIDData2 && KFSPartitionInfo->IsInit.Data3 == GUIDData3 && KFSPartitionInfo->IsInit.Data4 == GUIDData4) break;  
            
        }     
    }

    void KFS::InitKFS(){
        uint64_t BlockSize = blockSize;
        void* Block = mallocK(BlockSize);
        KFSinfo* info = (KFSinfo*)mallocK(sizeof(KFSinfo));
        uint64_t MemTotPartiton = (globalPartition->partition->LastLBA - globalPartition->partition->FirstLBA) * globalPartition->port->GetSectorSizeLBA();

        info->IsInit.Data1 = GUIDData1;
        info->IsInit.Data2 = GUIDData2;
        info->IsInit.Data3 = GUIDData3;
        info->IsInit.Data4 = GUIDData4;

        info->numberOfBlock = MemTotPartiton / BlockSize;        
        info->bitmapSizeByte = Divide(info->numberOfBlock, 8);
        info->BlockSize = BlockSize;
        info->bitmapSizeBlock = Divide(info->bitmapSizeByte, info->BlockSize);
        info->bitmapPosition = BlockSize;
        info->root.firstBlockForFile = info->bitmapPosition / BlockSize + info->bitmapSizeBlock;
        info->root.firstBlockFile = 0;
        info->root.fid = 0;
        globalPartition->Write(0, sizeof(KFSinfo), info);

        /* Clear Bitmap */
        memset(Block, 0, info->BlockSize);
        
        

        for(int i = 0; i < info->bitmapSizeBlock; i++){
            globalPartition->Write(info->bitmapPosition + (i * info->BlockSize), info->BlockSize, Block);
        }
        /* Lock KFSInfo */
        for(int i = 0; i < info->root.firstBlockForFile; i++){
            LockBlock(i);
        }

        freeK(Block);        
        freeK((void*)info);
        
        //reload info
        globalPartition->Read(0, sizeof(KFSinfo), KFSPartitionInfo);
    }   


    AllocatePartition* KFS::Allocate(size_t size, Folder* folder, uint64_t lastBlockRequested){
        uint64_t NumberBlockToAllocate = Divide(size, KFSPartitionInfo->BlockSize);
        uint64_t BlockAllocate = 0;
        uint64_t FirstBlocAllocated = 0;
        uint64_t NextBlock = 0;
        if(lastBlockRequested == 0){
            lastBlockRequested = KFSPartitionInfo->root.lastBlockAllocated;
        }        

        if(folder != NULL) lastBlockRequested = folder->folderInfo->lastBlockRequested;
        void* Block = mallocK(KFSPartitionInfo->BlockSize);
        void* BlockLast = mallocK(KFSPartitionInfo->BlockSize);

        for(int i = 0; i < NumberBlockToAllocate; i++){
            uint64_t BlockRequested = RequestBlock();
            if(BlockRequested != 0){
                BlockAllocate++;

                if(FirstBlocAllocated == 0){ //for the return value
                    FirstBlocAllocated = BlockRequested;
                }

                GetBlockData(BlockRequested, Block);
                BlockHeader* blockHeader = (BlockHeader*)Block;
                blockHeader->LastBlock = lastBlockRequested;
                blockHeader->NextBlock = 0;
                if(lastBlockRequested != 0){
                    KFS::GetBlockData(lastBlockRequested, BlockLast);
                    BlockHeader* blockHeaderLast = (BlockHeader*)BlockLast;
                    blockHeaderLast->NextBlock = BlockRequested;
                    memcpy(BlockLast, blockHeaderLast, sizeof(BlockHeader));
                    SetBlockData(lastBlockRequested, BlockLast);
                }
                
                memcpy(Block, blockHeader, sizeof(BlockHeader));
                SetBlockData(BlockRequested, Block);
                lastBlockRequested = BlockRequested;                 
            }else{
                break;
            }
        }
        if(BlockAllocate < NumberBlockToAllocate && BlockAllocate > 0){
            Free(FirstBlocAllocated, true);
            freeK(Block);
            freeK(BlockLast);
            return NULL;
        }

        /* Update lastblock requested */
        if(folder == NULL){
            KFSPartitionInfo->root.lastBlockAllocated = FirstBlocAllocated;
            UpdatePartitionInfo();
        }else{
            folder->folderInfo->lastBlockRequested = FirstBlocAllocated;
            UpdateFolderInfo(folder);
        }

        freeK(Block);
        freeK(BlockLast);
        AllocatePartition* allocatePartition = (AllocatePartition*)mallocK(sizeof(AllocatePartition));
        allocatePartition->FirstBlock = FirstBlocAllocated; 
        allocatePartition->LastBlock = lastBlockRequested;
        return allocatePartition;
    }

    void KFS::Free(uint64_t Block, bool DeleteData){
        uint64_t NextBlocToDelete = Block;
        BlockHeader* blockHeaderToDelete = (BlockHeader*)mallocK(sizeof(BlockHeader));
        void* BlankBuffer = mallocK(KFSPartitionInfo->BlockSize);
        while(true){
            KFS::GetBlockData(NextBlocToDelete, blockHeaderToDelete);                
            uint64_t temp = NextBlocToDelete;
            NextBlocToDelete = blockHeaderToDelete->NextBlock;
            if(DeleteData){
               SetBlockData(temp, BlankBuffer); 
            }
            
            UnlockBlock(temp);

            if(NextBlocToDelete == 0){
                break;
            }
        }
        
        freeK(blockHeaderToDelete);
    }

    uint64_t KFS::RequestBlock(){
        bool Check = false;
        void* BitmapBuffer = mallocK(KFSPartitionInfo->BlockSize);
        for(int i = 0; i < KFSPartitionInfo->bitmapSizeBlock; i++){
            for(int j = 0; j < KFSPartitionInfo->BlockSize; j++){
                uint8_t value = *(uint8_t*)((uint64_t)BitmapBuffer + j);
                if(value != uint8_Limit){ //so more than one block is free in this byte
                    for(int y = 0; y < 8; y++){
                        uint64_t Block = i * KFSPartitionInfo->BlockSize + j * 8 + y;
                        if(CheckBlock(Block)){ /* is free block */
                            LockBlock(Block);
                            freeK(BitmapBuffer);
                            return Block;
                        }
                    }
                }
            }
        } 
        freeK(BitmapBuffer);
        return 0;
    }

    void KFS::LockBlock(uint64_t Block){
        void* BitmapBuffer = mallocK(1); 
        globalPartition->Read(KFSPartitionInfo->bitmapPosition + (Block / 8), 1, BitmapBuffer);
        uint8_t value = *(uint8_t*)((uint64_t)BitmapBuffer);
        value = WriteBit(value, Block % 8, 1);
        *(uint8_t*)((uint64_t)BitmapBuffer) = value;
        globalPartition->Write(KFSPartitionInfo->bitmapPosition + (Block / 8), 1, BitmapBuffer);
        freeK(BitmapBuffer);
    }

    void KFS::UnlockBlock(uint64_t Block){
        void* BitmapBuffer = mallocK(1); 
        globalPartition->Read(KFSPartitionInfo->bitmapPosition + (Block / 8), 1, BitmapBuffer);
        uint8_t value = *(uint8_t*)((uint64_t)BitmapBuffer);
        value = WriteBit(value, Block % 8, 0);
        *(uint8_t*)((uint64_t)BitmapBuffer) = value;
        globalPartition->Write(KFSPartitionInfo->bitmapPosition + (Block / 8), 1, BitmapBuffer);
        freeK(BitmapBuffer);
    }

    bool KFS::CheckBlock(uint64_t Block){
        bool Check = false;
        void* BitmapBuffer = mallocK(1); 
        globalPartition->Read(KFSPartitionInfo->bitmapPosition + (Block / 8), 1, BitmapBuffer);
        uint8_t value = *(uint8_t*)((uint64_t)BitmapBuffer);
        
        freeK(BitmapBuffer); 
        Check = !ReadBit(value, Block % 8);
        return Check;
    }

    void KFS::GetBlockData(uint64_t Block, void* buffer){
        globalPartition->Read(Block * KFSPartitionInfo->BlockSize, KFSPartitionInfo->BlockSize, buffer);
    }

    void KFS::SetBlockData(uint64_t Block, void* buffer){
        globalPartition->Write(Block * KFSPartitionInfo->BlockSize, KFSPartitionInfo->BlockSize, buffer);
    }

    void KFS::Close(File* file){
        freeK(file);
    }

    void KFS::flist(char* filePath){
        char** FoldersSlit = split(filePath, "/");
        int count = 0;
        for(; FoldersSlit[count] != 0; count++);
        count--;

        

        if(KFSPartitionInfo->root.firstBlockFile == 0){
            printf("The disk is empty");
            return;
        }

        void* Block = mallocK(KFSPartitionInfo->BlockSize);
        uint64_t ScanBlock = KFSPartitionInfo->root.firstBlockFile;
        BlockHeader* ScanBlockHeader;
        HeaderInfo* ScanHeader;

        for(int i = 0; i <= count; i++){
            while(true){
                KFS::GetBlockData(ScanBlock, Block);
                ScanBlockHeader = (BlockHeader*)Block;
                ScanHeader = (HeaderInfo*)((uint64_t)Block + sizeof(BlockHeader));
             
                if(ScanHeader->IsFile){
                    FileInfo* fileInfo = (FileInfo*)((uint64_t)Block + sizeof(BlockHeader) + sizeof(HeaderInfo));
                    printf("File name : %s FID : %u Block : %u\n", fileInfo->name, ScanHeader->FID, ScanBlock); 
                    globalGraphics->Update();
                }else if(ScanHeader->FID != 0){
                    FolderInfo* folderInfo = (FolderInfo*)((uint64_t)Block + sizeof(BlockHeader) + sizeof(HeaderInfo));
                    printf("Folder name : %s", folderInfo->name);
                }

                ScanBlock = ScanBlockHeader->NextBlock;
                if(ScanBlock == 0){
                    break;
                }
            }
        }
        freeK((void*)Block);
    }

    uint64_t KFS::mkdir(char* filePath, uint64_t mode){
        Folder* folder = readdir(filePath);
        char** FoldersSlit = split(filePath, "/");
        int count;
        for(count = 0; FoldersSlit[count] != 0; count++);

        if(count != 0){ //if it isn't root
            if(folder->folderInfo != NULL){
                if(strcmp(folder->folderInfo->name, FoldersSlit[count])) return 0; //folder already exist
                if(!strcmp(folder->folderInfo->name, FoldersSlit[count - 1])) return 2; //folder before doesn't exist 
            }else{
                return 2; //folder before doesn't exist 
            }
        }
        uint64_t BlockSize = 2;
        uint64_t BlockSizeFolder = KFSPartitionInfo->BlockSize * BlockSize; //alloc one bloc, for the header and data
        uint64_t blockPosition = Allocate(BlockSizeFolder, folder, 0)->FirstBlock; 
        if(KFSPartitionInfo->root.firstBlockFile == 0){
            KFSPartitionInfo->root.firstBlockFile = blockPosition;
        }
        void* block = mallocK(KFSPartitionInfo->BlockSize);
        KFSStatic.KFS::GetBlockData(blockPosition, block);
        HeaderInfo* Header = (HeaderInfo*)(void*)((uint64_t)block + sizeof(BlockHeader));
        Header->FID = GetFID();
        Header->IsFile = true;
        FolderInfo* folderInfo = (FolderInfo*)(void*)((uint64_t)block + sizeof(BlockHeader) + sizeof(HeaderInfo));
        folderInfo->firstBlockData = ((BlockHeader*)block)->NextBlock;
        folderInfo->size = 0;
        folderInfo->FileBlockSize = BlockSize;

        for(int i = 0; i < MaxPath; i++){
            folderInfo->path[i] = filePath[i];
        }
        
        for(int i = 0; i < MaxName; i++){
            folderInfo->name[i] = FoldersSlit[count - 1][i];
        }

        RealTimeClock* realTimeClock;
        folderInfo->timeInfoFS.CreateTime.seconds = realTimeClock->readSeconds();
        folderInfo->timeInfoFS.CreateTime.minutes = realTimeClock->readMinutes();
        folderInfo->timeInfoFS.CreateTime.hours = realTimeClock->readHours();
        folderInfo->timeInfoFS.CreateTime.days = realTimeClock->readDay();
        folderInfo->timeInfoFS.CreateTime.months = realTimeClock->readMonth();
        folderInfo->timeInfoFS.CreateTime.years = realTimeClock->readYear() + 2000;

        folderInfo->mode = mode;

        memcpy((void*)((uint64_t)block + sizeof(BlockHeader)), Header, sizeof(HeaderInfo));
        memcpy((void*)((uint64_t)block + sizeof(BlockHeader) + sizeof(HeaderInfo)), folderInfo, sizeof(FolderInfo));
        
        SetBlockData(blockPosition, block);
        
        freeK(block);
        return 1; //sucess
    }

    KFS::Folder* KFS::readdir(char* filePath){
        char** FoldersSlit = split(filePath, "/");
        int count = 0;
        for(; FoldersSlit[count] != 0; count++);
        count--;

        if(KFSPartitionInfo->root.firstBlockFile == 0){
            printf("The disk is empty");
            return NULL;
        }

        void* Block = mallocK(KFSPartitionInfo->BlockSize);
        uint64_t ScanBlock = KFSPartitionInfo->root.firstBlockFile;
        BlockHeader* ScanBlockHeader;
        HeaderInfo* ScanHeader;

        Folder* folder = NULL;

        for(int i = 0; i <= count; i++){
            while(true){
                KFS::GetBlockData(ScanBlock, Block);
                ScanBlockHeader = (BlockHeader*)Block;
                ScanHeader = (HeaderInfo*)((uint64_t)Block + sizeof(BlockHeader));
             
                if(!ScanHeader->IsFile && ScanHeader->FID != 0){
                    FolderInfo* folderInfo = (FolderInfo*)((uint64_t)Block + sizeof(BlockHeader) + sizeof(HeaderInfo));
                    if(strcmp(folderInfo->name, FoldersSlit[i])){
                        if(folder != NULL){
                            freeK((void*)folder);
                        }
                        folder = (Folder*)mallocK(sizeof(Folder));
                    }
                }

                ScanBlock = ScanBlockHeader->NextBlock;
                if(ScanBlock == 0){
                    break;
                }
            }
        }
        freeK((void*)Block);
        return folder;
    }

    KFS::File* KFS::fopen(char* filePath, char* mode){
        char** FoldersSlit = split(filePath, "/");
        int count = 0;
        for(; FoldersSlit[count] != 0; count++);
        count--;
        void* Block = mallocK(KFSPartitionInfo->BlockSize);
        char* FileName;
        char* FileNameSearch = FoldersSlit[count];
        uint64_t ScanBlock = KFSPartitionInfo->root.firstBlockFile;
        BlockHeader* ScanBlockHeader;
        HeaderInfo* ScanHeader;

        Folder* folder = NULL;

        File* returnData = NULL;

        if(KFSPartitionInfo->root.firstBlockFile == 0 && count == 0){
            freeK((void*)Block);
            returnData = (File*)mallocK(sizeof(File));
            returnData->fileInfo = NewFile(filePath, folder);
            returnData->mode = mode;
            return returnData;
        }else if(KFSPartitionInfo->root.firstBlockFile == 0 && count != 0){
            freeK((void*)Block);
            return NULL;
        }

        for(int i = 0; i <= count; i++){
            while(true){
                KFSStatic.KFS::GetBlockData(ScanBlock, Block);
                ScanBlockHeader = (BlockHeader*)Block;
                ScanHeader = (HeaderInfo*)((uint64_t)Block + sizeof(BlockHeader));
             
                if(ScanHeader->IsFile){
                    FileInfo* fileInfo = (FileInfo*)((uint64_t)Block + sizeof(BlockHeader) + sizeof(HeaderInfo));
                    FileName = fileInfo->name;
        
                    int FileNameLen = strlen(FileName);
                    int FileNameTempLen = strlen(FoldersSlit[count]);
                    if(i == count && strcmp(FileName, FoldersSlit[count])){
                        freeK((void*)Block);

                        returnData = (File*)mallocK(sizeof(File));
                        returnData->fileInfo = fileInfo;
                        returnData->mode = mode;
                        if(folder != NULL){
                            freeK((void*)folder);
                        }
                        return returnData;
                    }
                }else if(ScanHeader->FID != 0){
                    FolderInfo* folderInfo = (FolderInfo*)((uint64_t)Block + sizeof(BlockHeader) + sizeof(HeaderInfo));
                    FileName = folderInfo->name;
                    if(i == count){
                        returnData = NULL;
                    }

                    if(FileName == FoldersSlit[i]){
                        ScanBlock = folderInfo->firstBlockData;
                        if(folder != NULL){
                            freeK((void*)folder);
                        }
                        folder = (Folder*)mallocK(sizeof(Folder));
                        break;
                    }
                }

                ScanBlock = ScanBlockHeader->NextBlock;
                if(ScanBlock == 0){
                    returnData = NULL;
                    if(i == count){
                        freeK((void*)Block);
                        returnData = (File*)mallocK(sizeof(File));
                        returnData->fileInfo = NewFile(filePath, folder);
                        returnData->mode = mode;

                        if(folder != NULL){
                            freeK((void*)folder);
                        }

                        return returnData;
                    }
                }else{
                    returnData = NULL;
                }
            }            
        } 

        if(folder != NULL){
            freeK((void*)folder);
        }
        freeK((void*)Block);
        return returnData;
    }

    FileInfo* KFS::NewFile(char* filePath, Folder* folder){
        printf("New file: %s\n", filePath);
        uint64_t BlockSize = 2;
        uint64_t FileBlockSize = KFSPartitionInfo->BlockSize * BlockSize; //alloc one bloc, for the header and data
        AllocatePartition* allocatePartition = Allocate(FileBlockSize, folder, 0);
        uint64_t BlockLastAllocate = allocatePartition->LastBlock;
        uint64_t blockPosition = allocatePartition->FirstBlock; 
        freeK(allocatePartition);
        if(KFSPartitionInfo->root.firstBlockFile == 0){
            KFSPartitionInfo->root.firstBlockFile = blockPosition;
        }
        void* block = mallocK(KFSPartitionInfo->BlockSize);
        GetBlockData(blockPosition, block);
        HeaderInfo* Header = (HeaderInfo*)(void*)((uint64_t)block + sizeof(BlockHeader));
        Header->FID = GetFID();
        Header->IsFile = true;
        Header->ParentLocationBlock = folder->folderInfo->BlockHeader;
        FileInfo* fileInfo = (FileInfo*)(void*)((uint64_t)block + sizeof(BlockHeader) + sizeof(HeaderInfo));
        fileInfo->BlockHeader = blockPosition;
        fileInfo->firstBlockData = ((BlockHeader*)block)->NextBlock;
        fileInfo->size = 0;
        fileInfo->FileBlockSize = BlockSize;
        fileInfo->FileBlockData = BlockSize + 1; //the header use 1 block
        fileInfo->lastBlock = BlockLastAllocate;

        for(int i = 0; i < MaxPath; i++){
            fileInfo->path[i] = filePath[i];
        }
        

        char** FoldersSlit = split(filePath, "/");
        int count;
        for(count = 0; FoldersSlit[count] != 0; count++);
        for(int i = 0; i < MaxName; i++){
            fileInfo->name[i] = FoldersSlit[count - 1][i];
        }

        RealTimeClock* realTimeClock;
        fileInfo->timeInfoFS.CreateTime.seconds = realTimeClock->readSeconds();
        fileInfo->timeInfoFS.CreateTime.minutes = realTimeClock->readMinutes();
        fileInfo->timeInfoFS.CreateTime.hours = realTimeClock->readHours();
        fileInfo->timeInfoFS.CreateTime.days = realTimeClock->readDay();
        fileInfo->timeInfoFS.CreateTime.months = realTimeClock->readMonth();
        fileInfo->timeInfoFS.CreateTime.years = realTimeClock->readYear() + 2000;

        memcpy((void*)((uint64_t)block + sizeof(BlockHeader)), Header, sizeof(HeaderInfo));
        memcpy((void*)((uint64_t)block + sizeof(BlockHeader) + sizeof(HeaderInfo)), fileInfo, sizeof(FileInfo));
        SetBlockData(blockPosition, block);
        freeK(block);
        return fileInfo;
    }

    uint64_t KFS::GetFID(){
        KFSPartitionInfo->root.fid++;
        while(!UpdatePartitionInfo());
        return KFSPartitionInfo->root.fid;
    }

    bool KFS::UpdatePartitionInfo(){
        return globalPartition->Write(0, sizeof(KFSinfo), KFSPartitionInfo);
    }

    void KFS::UpdateFolderInfo(Folder* folder){
        SetBlockData(folder->folderInfo->BlockHeader, folder->folderInfo);
    }

    void KFS::UpdateFileInfo(File* file){
        SetBlockData(file->fileInfo->BlockHeader, file->fileInfo);
    }

    uint64_t KFS::File::Read(uint64_t start, size_t size, void* buffer){
        void* Block = mallocK(KFS::KFSPartitionInfo->BlockSize);

        uint64_t BlockStart = start / KFS::KFSPartitionInfo->BlockSize;
        uint64_t BlockCount = Divide(size, KFS::KFSPartitionInfo->BlockSize - sizeof(BlockHeader));
        
        uint64_t FirstByte = start % KFS::KFSPartitionInfo->BlockSize;
        uint64_t ReadBlock = fileInfo->firstBlockData;
        uint64_t bytesRead = 0;
        uint64_t bytesToRead = 0;
        uint64_t blockRead = 0;

        //find the start Block
        for(int i = 0; i < BlockStart; i++){
            KFSStatic.KFS::GetBlockData(ReadBlock, Block);

            ReadBlock = ((BlockHeader*)Block)->NextBlock;

            if(ReadBlock == 0){
                return 0; //end of file before end of read
            }
        }

        for(int i = 0; i < BlockCount; i++){
            bytesToRead = size - bytesRead;
            if(bytesToRead > KFS::KFSPartitionInfo->BlockSize){
                bytesToRead = KFS::KFSPartitionInfo->BlockSize;
            }

            printf("%k %u %k", 0xffffffff00, ReadBlock, 0xffffffffff);
            KFSStatic.KFS::GetBlockData(ReadBlock, Block);

            ReadBlock = ((BlockHeader*)Block)->NextBlock;

            if(ReadBlock == 0){
                return 0; //end of file before end of read
            }

            if(bytesRead != 0){
                memcpy((void*)((uint64_t)buffer + bytesRead), Block + sizeof(BlockHeader), bytesToRead);
            }else{
                memcpy(buffer, (void*)((uint64_t)Block + sizeof(BlockHeader) + FirstByte), bytesToRead); //Get the correct first byte
            }

            bytesRead += KFS::KFSPartitionInfo->BlockSize;
        }

        freeK(Block);

        if(BlockStart + BlockCount > fileInfo->FileBlockSize){
            return 2; //size too big
        }
    }

    uint64_t KFS::File::Write(uint64_t start, size_t size, void* buffer){
        //let's check if we need to enlarge the file or shrink it
        void* Block = mallocK(KFS::KFSPartitionInfo->BlockSize);
        printf("%u", KFS::KFSPartitionInfo->BlockSize);
        freeK(Block);
        return 1;
        uint64_t BlockStart = start / KFS::KFSPartitionInfo->BlockSize - sizeof(BlockHeader);
        uint64_t BlockCount = Divide(size, KFS::KFSPartitionInfo->BlockSize - sizeof(BlockHeader));
        uint64_t BlockTotal = BlockStart + BlockCount;

        if(BlockTotal != fileInfo->FileBlockData){
            if(BlockTotal > fileInfo->FileBlockData){
                size_t NewSize = BlockTotal * KFS::KFSPartitionInfo->BlockSize - sizeof(BlockHeader);
                //Aloc new blocks
                fileInfo->lastBlock = KFSStatic.KFS::Allocate(NewSize, NULL, fileInfo->lastBlock)->LastBlock;
                fileInfo->size = NewSize;
                fileInfo->FileBlockData = BlockTotal;
                fileInfo->FileBlockSize = BlockTotal + 1;
                UpdateFileInfo(this);
            }else{
                size_t NewSize = BlockTotal * KFS::KFSPartitionInfo->BlockSize - sizeof(BlockHeader);
                //Free last block
                KFSStatic.KFS::Free(fileInfo->firstBlockData + BlockTotal, true);
                fileInfo->size = NewSize;
                fileInfo->FileBlockData = BlockTotal;
                fileInfo->FileBlockSize = BlockTotal + 1;
                UpdateFileInfo(this);
            }
        }

        uint64_t FirstByte = start % KFS::KFSPartitionInfo->BlockSize - sizeof(BlockHeader);
        uint64_t WriteBlock = fileInfo->firstBlockData;
        uint64_t bytesWrite = 0;
        uint64_t bytesToWrite = 0;
        uint64_t blockWrite = 0;

        //find the start Block
        for(int i = 0; i < BlockStart; i++){
            KFSStatic.KFS::GetBlockData(WriteBlock, Block);

            WriteBlock = ((BlockHeader*)Block)->NextBlock;

            if(WriteBlock == 0){
                return 0; //end of file before end of read
            }
        }

        for(int i = 0; i < BlockCount; i++){
            bytesToWrite = size - bytesWrite;
            if(bytesToWrite > KFS::KFSPartitionInfo->BlockSize){
                bytesToWrite = KFS::KFSPartitionInfo->BlockSize;
            }

            printf("%k %u %k", 0xffffffff00, WriteBlock, 0xffffffffff);
            KFSStatic.KFS::SetBlockData(WriteBlock, Block);
            if(bytesWrite != 0){
                memcpy(Block, (void*)((uint64_t)buffer + sizeof(BlockHeader) + bytesWrite), bytesToWrite);
            }else{
                memcpy((void*)((uint64_t)Block + sizeof(BlockHeader) + FirstByte), buffer, bytesToWrite); //Get the correct first byte
            }

            KFSStatic.KFS::GetBlockData(WriteBlock, Block);

            WriteBlock = ((BlockHeader*)Block)->NextBlock;

            if(WriteBlock == 0){
                return 0; //end of file before end of read
            }            

            bytesWrite += KFS::KFSPartitionInfo->BlockSize;
        }

        freeK(Block);
        return 1;
    }
}