/*
 * Compile: g++ -std=c++17 -static -static-libgcc -static-libstdc++ -pthread src/mount.cpp src/lstream.cpp -o bin/mount.exe 2>&1
 * Leveled File System Shell - Level-First Architecture
 */

#include "fs_common.hpp"
#include "journal.hpp"
#include "permissions.hpp"
#include "fs_entry.hpp"
#include "fs_context.hpp"
#include "defrag.hpp"
#include "concurrent.hpp"
#include "lstream.hpp"
#include "utils/handler.hpp"
#include "utils/handler.cpp"

class FileSystemShell {
    DiskDevice disk;
    SuperBlock sb;
    Journal* journal;
    FileLockManager lockManager;
    
    ThreadPool threadPool;
    TaskHandler taskHandler;
    
    PermissionCache permCache;
    EntryReader* entryReader;
    EntryWriter* entryWriter;
    EntryFinder* entryFinder;
    
    NavigationContext context;
    ContextManager* ctxManager;
    PermissionResolver* permResolver;
    
    map<string, string> variables;  // Variable storage for $var expansion

public:
    FileSystemShell() : journal(nullptr), entryReader(nullptr), entryWriter(nullptr), entryFinder(nullptr), ctxManager(nullptr), permResolver(nullptr) {
        ctxManager = new ContextManager(context, disk);
    }
    
    ~FileSystemShell() {
        if (journal) delete journal;
        if (entryReader) delete entryReader;
        if (entryWriter) delete entryWriter;
        if (entryFinder) delete entryFinder;
        if (ctxManager) delete ctxManager;
        if (permResolver) delete permResolver;
    }

    LevelDescriptor* findLevelByID(uint64_t levelID) {
        if (sb.levelRegistryCluster == 0) return nullptr;
        
        static LevelDescriptor registry[SECTOR_SIZE / sizeof(LevelDescriptor)];
        vector<uint64_t> chain = getChain(sb.levelRegistryCluster);
        
        for (uint64_t c : chain) {
            for (int s = 0; s < 8; s++) {
                disk.readSector(c * 8 + s, registry);
                for (int j = 0; j < SECTOR_SIZE / sizeof(LevelDescriptor); j++) {
                    if (registry[j].levelID == levelID && (registry[j].flags & LEVEL_FLAG_ACTIVE)) {
                        return &registry[j];
                    }
                }
            }
        }
        return nullptr;
    }
    
    LevelDescriptor* findLevelByName(const string& name) {
        if (sb.levelRegistryCluster == 0) return nullptr;
        
        static LevelDescriptor registry[SECTOR_SIZE / sizeof(LevelDescriptor)];
        vector<uint64_t> chain = getChain(sb.levelRegistryCluster);
        
        for (uint64_t c : chain) {
            for (int s = 0; s < 8; s++) {
                disk.readSector(c * 8 + s, registry);
                for (int j = 0; j < SECTOR_SIZE / sizeof(LevelDescriptor); j++) {
                    if ((registry[j].flags & LEVEL_FLAG_ACTIVE) && string(registry[j].name) == name) {
                        return &registry[j];
                    }
                }
            }
        }
        return nullptr;
    }

    bool mount(char driveLetter) {
        string path = "\\\\.\\";
        path += driveLetter;
        path += ":";
        
        HANDLE hTest = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, 
                                   OPEN_EXISTING, 0, NULL);
        
        if (hTest == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
                lout << "Disk/Partition " << driveLetter << ": doesn't exist.\n";
            } else if (err == ERROR_ACCESS_DENIED) {
                lout << "Disk/Partition " << driveLetter << ": can't be accessed.\n";
            } else {
                lout << "Disk/Partition " << driveLetter << ": can't be accessed. (Error " << err << ")\n";
            }
            return false;
        }
        CloseHandle(hTest);
        
        if (!disk.open(driveLetter)) {
            lout << "Disk/Partition " << driveLetter << ": can't be accessed.\n";
            return false;
        }
        
        if (!disk.readSector(0, &sb)) {
            lout << "Disk/Partition " << driveLetter << ": can't be accessed.\n";
            disk.close();
            return false;
        }
        if (sb.magic != MAGIC) {
            if (!tryBackupSuperblock()) {
                lout << "Disk/Partition " << driveLetter << ": is not a LeveledFS.\n";
                disk.close();
                return false;
            }
        }

        journal = new Journal(&disk, &sb);
        journal->replayJournal();

        context.currentDirCluster = sb.rootDirCluster;
        context.currentPath = "/";
        context.rootLevelID = sb.rootLevelID;
        context.currentFolderPerms = PERM_ROOT_DEFAULT;
        
        if (entryReader) delete entryReader;
        if (entryWriter) delete entryWriter;
        if (entryFinder) delete entryFinder;
        entryReader = new EntryReader(disk);
        entryWriter = new EntryWriter(disk);
        entryFinder = new EntryFinder(disk);
        permCache.clear();
        
        lout << "Mounted successfully. At Root.\n";
        
        if(loadVersion("master")) {
            lout << "Context: master (Level ID: " << context.currentLevelID << ")\n";
            context.rootContentCluster = context.currentContentCluster;
            context.rootVersion = "master";
            
            if (permResolver) delete permResolver;
            permResolver = new PermissionResolver(disk, permCache, context.rootContentCluster);
            
            if (!isRootFSInitialized()) {
                rootfsInit();
            }
            loadVariables();
            
            taskHandler.startOptimizer([this]() {
            }, 60000);
        } else {
            lout << "No master version.\n";
            context.rootContentCluster = 0;
        }
        return true;
    }
    
    bool mountAuto() {
        lout << "Scanning for LevelFS volumes...\n";
        
        for (char letter = 'A'; letter <= 'Z'; letter++) {
            if (letter == 'C') continue;
            
            DiskDevice testDisk;
            if (!testDisk.open(letter)) continue;
            
            SuperBlock testSb;
            if (!testDisk.readSector(0, &testSb)) {
                testDisk.close();
                continue;
            }
            
            if (testSb.magic == MAGIC) {
                testDisk.close();
                lout << "Found LevelFS volume on " << letter << ":\n";
                return mount(letter);
            }
            
            testDisk.close();
        }
        
        lout << "No drive letter volumes found. Scanning physical disks...\n";
        for (int diskNum = 0; diskNum <= 9; diskNum++) {
            string diskPath = "\\\\.\\PhysicalDrive" + to_string(diskNum);
            
            static const uint64_t partitionOffsets[] = {
                0,              // Whole-disk format
                2048,           // GPT partition (1MB offset)
                63,             // Legacy MBR partition
                128,            // Some USB drives
            };
            
            for (uint64_t offsetSectors : partitionOffsets) {
                uint64_t offsetBytes = offsetSectors * SECTOR_SIZE;
                
                DiskDevice testDisk;
                if (!testDisk.open(diskPath, offsetBytes)) continue;
                
                SuperBlock testSb;
                if (!testDisk.readSector(0, &testSb)) {
                    testDisk.close();
                    continue;
                }
                
                if (testSb.magic == MAGIC) {
                    testDisk.close();
                    lout << "Found LevelFS on " << diskPath << " at offset " << offsetBytes << " bytes\n";
                    return mountPhysicalWithOffset(diskPath, offsetBytes);
                }
                
                testDisk.close();
            }
        }
        
        lout << "No LevelFS disk found.\n";
        return false;
    }

    bool mountPhysical(const string& diskPath) {
        return mountPhysicalWithOffset(diskPath, 0);
    }

    bool mountPhysicalWithOffset(const string& diskPath, uint64_t offsetBytes) {
        if (!disk.open(diskPath, offsetBytes)) {
            lout << "Failed to open " << diskPath << " at offset " << offsetBytes << "\n";
            return false;
        }
        
        if (!disk.readSector(0, &sb)) {
            lout << "Failed to read superblock.\n";
            disk.close();
            return false;
        }
        if (sb.magic != MAGIC) {
            if (!tryBackupSuperblock()) {
                lout << "Invalid magic: " << hex << sb.magic << dec << "\n";
                disk.close();
                return false;
            }
        }

        journal = new Journal(&disk, &sb);
        journal->replayJournal();

        context.currentDirCluster = sb.rootDirCluster;
        context.currentPath = "/";
        context.rootLevelID = sb.rootLevelID;
        context.currentFolderPerms = PERM_ROOT_DEFAULT;
        
        if (entryReader) delete entryReader;
        if (entryWriter) delete entryWriter;
        if (entryFinder) delete entryFinder;
        entryReader = new EntryReader(disk);
        entryWriter = new EntryWriter(disk);
        entryFinder = new EntryFinder(disk);
        permCache.clear();
        
        if(loadVersion("master")) {
            context.rootContentCluster = context.currentContentCluster;
            context.rootVersion = "master";
        } else {
            context.rootContentCluster = 0;
        }
        
        lout << "=== Leveled File System v2 ===\n";
        lout << "  Volume: " << sb.volumeName << "\n";
        lout << "  Source: " << diskPath << "\n";
        if (offsetBytes > 0) {
            lout << "  Partition Offset: " << offsetBytes << " bytes\n";
        }
        lout << "  Total Clusters: " << sb.totalClusters << "\n";
        lout << "  Free Clusters: " << sb.totalFreeClusters << "\n";
        lout << "  Root Level: " << context.rootVersion << "\n";
        
        if (permResolver) delete permResolver;
        permResolver = new PermissionResolver(disk, permCache, context.rootContentCluster);
        
        if (!isRootFSInitialized()) {
            rootfsInit();
        }
        loadVariables();
        
        return true;
    }

    bool mountImage(const string& path) {
        if (!disk.openFile(path)) {
            lout << "Failed to open image file: " << path << "\n";
            return false;
        }
        
        if (!disk.readSector(0, &sb)) {
            lout << "Failed to read superblock.\n";
            disk.close();
            return false;
        }
        if (sb.magic != MAGIC) {
            if (!tryBackupSuperblock()) {
                lout << "Invalid magic: " << hex << sb.magic << dec << "\n";
                disk.close();
                return false;
            }
        }

        journal = new Journal(&disk, &sb);
        journal->replayJournal();

        context.currentDirCluster = sb.rootDirCluster;
        context.currentPath = "/";
        context.rootLevelID = sb.rootLevelID;
        context.currentFolderPerms = PERM_ROOT_DEFAULT;
        
        if (entryReader) delete entryReader;
        if (entryWriter) delete entryWriter;
        if (entryFinder) delete entryFinder;
        entryReader = new EntryReader(disk);
        entryWriter = new EntryWriter(disk);
        entryFinder = new EntryFinder(disk);
        permCache.clear();
        
        lout << "=== Leveled File System v2 ===\n";
        lout << "  Image: " << path << " (" << (disk.getDiskSize()/1024/1024) << " MB)\n";
        
        if (sb.levelRegistryCluster != 0) {
            lout << "  Level Registry: Cluster " << sb.levelRegistryCluster << "\n";
            lout << "  Total Levels: " << sb.totalLevels << "\n";
        }
        
        if(loadVersion("master")) {
            lout << "  Active Level: master (ID: " << context.currentLevelID << ")\n";
            context.rootContentCluster = context.currentContentCluster;
        } else {
        lout << "No master version found. Creating...\n";
        char vpsBuf[SECTOR_SIZE];
        VersionEntry* vps = (VersionEntry*)vpsBuf;
        memset(vpsBuf, 0, SECTOR_SIZE);
        strcpy(vps[0].versionName, "master");
        vps[0].isActive = 1;
        vps[0].contentTableCluster = allocCluster();
        vps[0].levelID = LEVEL_ID_MASTER;
        vps[0].parentLevelID = LEVEL_ID_NONE;
        vps[0].flags = LEVEL_FLAG_ACTIVE;
        disk.writeSector(sb.rootDirCluster * 8, vpsBuf);
        loadVersion("master");
        context.rootContentCluster = context.currentContentCluster;
    }
        
        if (!isRootFSInitialized()) {
            rootfsInit();
        }
        loadVariables();
        
        return true;
    }
    
    bool isReservedCluster(uint64_t cluster) {
        if (cluster == 0) return true;
        if (cluster == sb.backupSBCluster) return true;
        if (cluster >= sb.litStartCluster && cluster < sb.litStartCluster + sb.litClusters) return true;
        if (cluster >= sb.labPoolStart && cluster < sb.labPoolStart + sb.labPoolClusters) return true;
        if (cluster >= sb.levelRegistryCluster && cluster < sb.levelRegistryCluster + sb.levelRegistryClusters) return true;
        if (cluster >= sb.journalStartCluster && cluster < sb.journalStartCluster + (sb.journalSectors / SECTORS_PER_CLUSTER + 1)) return true;
        if (cluster >= sb.rootDirCluster && cluster <= sb.rootDirCluster + 1) return true;
        return false;
    }

    LABEntry getLABEntry(uint64_t cluster) {
        LABEntry result;
        memset(&result, 0, sizeof(result));
        result.nextCluster = LAT_FREE;
        result.levelID = LEVEL_ID_NONE;
        result.flags = 0;
        result.refCount = 0;
        
        if (cluster == 0 || cluster >= sb.totalClusters) {
            return result;
        }
        
        if (isReservedCluster(cluster)) {
            result.nextCluster = LAT_END;
            return result;
        }
        
        uint64_t litIndex = cluster / CLUSTERS_PER_LIT_ENTRY;
        uint64_t labOffset = cluster % CLUSTERS_PER_LIT_ENTRY;
        
        uint64_t litClusterIdx = litIndex / (CLUSTER_SIZE / sizeof(LITEntry));
        uint64_t litEntryIdx = litIndex % (CLUSTER_SIZE / sizeof(LITEntry));
        
        if (sb.litStartCluster + litClusterIdx >= sb.totalClusters) {
            return result;
        }
        
        char* litBuffer = new char[CLUSTER_SIZE];
        for (int s = 0; s < SECTORS_PER_CLUSTER; s++) {
            disk.readSector((sb.litStartCluster + litClusterIdx) * SECTORS_PER_CLUSTER + s,
                litBuffer + s * SECTOR_SIZE);
        }
        LITEntry* litEntries = (LITEntry*)litBuffer;
        
        uint64_t labCluster = litEntries[litEntryIdx].labCluster;
        delete[] litBuffer;
        
        if (labCluster == LIT_EMPTY || labCluster == 0) {
            return result;
        }
        
        if (labCluster < sb.labPoolStart || labCluster >= sb.labPoolStart + sb.labPoolClusters) {
            return result;
        }
        
        char* labBuffer = new char[CLUSTER_SIZE];
        for (int s = 0; s < SECTORS_PER_CLUSTER; s++) {
            disk.readSector(labCluster * SECTORS_PER_CLUSTER + s,
                labBuffer + s * SECTOR_SIZE);
        }
        LABEntry* labEntries = (LABEntry*)labBuffer;
        
        result = labEntries[labOffset];
        delete[] labBuffer;
        return result;
    }
    
    uint64_t getLATEntry(uint64_t cluster) {
        if (cluster == 0 || cluster >= sb.totalClusters) return LAT_END;
        if (isReservedCluster(cluster)) return LAT_END;
        LABEntry entry = getLABEntry(cluster);
        return entry.nextCluster;
    }
    
    void setLABEntry(uint64_t cluster, LABEntry value) {
        uint64_t litIndex = cluster / CLUSTERS_PER_LIT_ENTRY;
        uint64_t labOffset = cluster % CLUSTERS_PER_LIT_ENTRY;
        
        uint64_t litClusterIdx = litIndex / (CLUSTER_SIZE / sizeof(LITEntry));
        uint64_t litEntryIdx = litIndex % (CLUSTER_SIZE / sizeof(LITEntry));
        
        char* litBuffer = new char[CLUSTER_SIZE];
        for (int s = 0; s < SECTORS_PER_CLUSTER; s++) {
            disk.readSector((sb.litStartCluster + litClusterIdx) * SECTORS_PER_CLUSTER + s,
                litBuffer + s * SECTOR_SIZE);
        }
        LITEntry* litEntries = (LITEntry*)litBuffer;
        
        uint64_t labCluster = litEntries[litEntryIdx].labCluster;
        
        if (labCluster == LIT_EMPTY || labCluster == 0) {
            uint64_t newLABCluster = sb.labPoolStart + sb.nextFreeLAB;
            sb.nextFreeLAB++;
            
            char* newLABBuffer = new char[CLUSTER_SIZE];
            memset(newLABBuffer, 0, CLUSTER_SIZE);
            LABEntry* newLAB = (LABEntry*)newLABBuffer;
            for (int i = 0; i < LAB_ENTRIES_PER_CLUSTER; i++) {
                newLAB[i].nextCluster = LAT_FREE;
                newLAB[i].levelID = LEVEL_ID_NONE;
                newLAB[i].flags = 0;
                newLAB[i].refCount = 0;
            }
            for (int s = 0; s < SECTORS_PER_CLUSTER; s++) {
                disk.writeSector(newLABCluster * SECTORS_PER_CLUSTER + s,
                    newLABBuffer + s * SECTOR_SIZE);
            }
            delete[] newLABBuffer;
            
            litEntries[litEntryIdx].labCluster = newLABCluster;
            litEntries[litEntryIdx].baseCluster = litIndex * CLUSTERS_PER_LIT_ENTRY;
            litEntries[litEntryIdx].allocatedCount = 0;
            litEntries[litEntryIdx].flags = 0;
            labCluster = newLABCluster;
            
            for (int s = 0; s < SECTORS_PER_CLUSTER; s++) {
                disk.writeSector((sb.litStartCluster + litClusterIdx) * SECTORS_PER_CLUSTER + s,
                    litBuffer + s * SECTOR_SIZE);
            }
            
            writeSuperBlock();
        }
        
        delete[] litBuffer;
        
        char* labBuffer = new char[CLUSTER_SIZE];
        for (int s = 0; s < SECTORS_PER_CLUSTER; s++) {
            disk.readSector(labCluster * SECTORS_PER_CLUSTER + s,
                labBuffer + s * SECTOR_SIZE);
        }
        LABEntry* labEntries = (LABEntry*)labBuffer;
        
        labEntries[labOffset] = value;
        
        for (int s = 0; s < SECTORS_PER_CLUSTER; s++) {
            disk.writeSector(labCluster * SECTORS_PER_CLUSTER + s,
                labBuffer + s * SECTOR_SIZE);
        }
        delete[] labBuffer;
    }

    void setLATEntry(uint64_t cluster, uint64_t value) {
        LABEntry entry = getLABEntry(cluster);
        entry.nextCluster = value;
        if (value == LAT_END) entry.flags |= LAT_FLAG_USED;
        setLABEntry(cluster, entry);
    }
    
    void setLATEntryWithLevel(uint64_t cluster, uint64_t value, uint32_t levelID) {
        LABEntry entry;
        entry.nextCluster = value;
        entry.levelID = levelID;
        entry.flags = LAT_FLAG_USED;
        entry.refCount = 1;
        setLABEntry(cluster, entry);
    }
    
    void updateLITAllocatedCount(uint64_t cluster, int delta) {
        uint64_t litIndex = cluster / CLUSTERS_PER_LIT_ENTRY;
        uint64_t litClusterIdx = litIndex / LIT_ENTRIES_PER_CLUSTER;
        uint64_t litEntryIdx = litIndex % LIT_ENTRIES_PER_CLUSTER;
        
        if (litClusterIdx >= sb.litClusters) return;
        
        char* litBuffer = new char[CLUSTER_SIZE];
        for (int s = 0; s < SECTORS_PER_CLUSTER; s++) {
            disk.readSector((sb.litStartCluster + litClusterIdx) * SECTORS_PER_CLUSTER + s,
                litBuffer + s * SECTOR_SIZE);
        }
        LITEntry* litEntries = (LITEntry*)litBuffer;
        
        if (litEntries[litEntryIdx].labCluster != 0) {
            if (delta > 0) {
                litEntries[litEntryIdx].allocatedCount += delta;
            } else if (litEntries[litEntryIdx].allocatedCount >= (uint32_t)(-delta)) {
                litEntries[litEntryIdx].allocatedCount += delta;
            }
            
            for (int s = 0; s < SECTORS_PER_CLUSTER; s++) {
                disk.writeSector((sb.litStartCluster + litClusterIdx) * SECTORS_PER_CLUSTER + s,
                    litBuffer + s * SECTOR_SIZE);
            }
        }
        delete[] litBuffer;
    }
    
    void updateLevelStats(uint64_t levelID, int64_t sizeDelta, int childDelta) {
        if (sb.levelRegistryCluster == 0) return;
        
        vector<uint64_t> chain = getChain(sb.levelRegistryCluster);
        for (uint64_t c : chain) {
            char registryBuf[CLUSTER_SIZE];
            for (int s = 0; s < SECTORS_PER_CLUSTER; s++) {
                disk.readSector(c * SECTORS_PER_CLUSTER + s, registryBuf + s * SECTOR_SIZE);
            }
            LevelDescriptor* registry = (LevelDescriptor*)registryBuf;
            
            for (int j = 0; j < CLUSTER_SIZE / sizeof(LevelDescriptor); j++) {
                if (registry[j].levelID == levelID && (registry[j].flags & LEVEL_FLAG_ACTIVE)) {
                    if (sizeDelta != 0) {
                        if (sizeDelta > 0) {
                            registry[j].totalSize += sizeDelta;
                        } else if (registry[j].totalSize >= (uint64_t)(-sizeDelta)) {
                            registry[j].totalSize += sizeDelta;
                        }
                    }
                    if (childDelta != 0) {
                        if (childDelta > 0) {
                            registry[j].childCount += childDelta;
                        } else if (registry[j].childCount >= (uint64_t)(-childDelta)) {
                            registry[j].childCount += childDelta;
                        }
                    }
                    registry[j].modTime = time(0);
                    
                    for (int s = 0; s < SECTORS_PER_CLUSTER; s++) {
                        disk.writeSector(c * SECTORS_PER_CLUSTER + s, registryBuf + s * SECTOR_SIZE);
                    }
                    return;
                }
            }
        }
    }
    
    void freeCluster(uint64_t cluster) {
        if (cluster == 0 || isReservedCluster(cluster)) return;
        
        updateLITAllocatedCount(cluster, -1);
        
        LABEntry entry;
        entry.nextCluster = LAT_FREE;
        entry.levelID = LEVEL_ID_NONE;
        entry.flags = 0;
        entry.refCount = 0;
        setLABEntry(cluster, entry);
        
        sb.totalFreeClusters++;
        writeSuperBlock();
    }
    
    void freeChain(uint64_t startCluster) {
        if (startCluster == 0 || isReservedCluster(startCluster)) return;
        
        vector<uint64_t> chain = getChain(startCluster);
        for (uint64_t c : chain) {
            updateLITAllocatedCount(c, -1);
            LABEntry entry;
            entry.nextCluster = LAT_FREE;
            entry.levelID = LEVEL_ID_NONE;
            entry.flags = 0;
            entry.refCount = 0;
            setLABEntry(c, entry);
            sb.totalFreeClusters++;
        }
        writeSuperBlock();
    }

    uint64_t allocCluster() {
        return allocClusterForLevel(context.currentLevelID);
    }
    
    uint64_t skipPastReserved(uint64_t c) {
        if (c == 0) return 1;
        if (c == sb.backupSBCluster) return c + 1;
        if (c >= sb.litStartCluster && c < sb.litStartCluster + sb.litClusters) 
            return sb.litStartCluster + sb.litClusters;
        if (c >= sb.labPoolStart && c < sb.labPoolStart + sb.labPoolClusters) 
            return sb.labPoolStart + sb.labPoolClusters;
        if (c >= sb.levelRegistryCluster && c < sb.levelRegistryCluster + sb.levelRegistryClusters) 
            return sb.levelRegistryCluster + sb.levelRegistryClusters;
        if (c >= sb.journalStartCluster && c < sb.journalStartCluster + (sb.journalSectors / SECTORS_PER_CLUSTER + 1)) 
            return sb.journalStartCluster + (sb.journalSectors / SECTORS_PER_CLUSTER + 1);
        if (c >= sb.rootDirCluster && c <= sb.rootDirCluster + 1) 
            return sb.rootDirCluster + 2;
        return c;
    }
    
    uint64_t allocClusterForLevel(uint32_t levelID) {
        uint64_t c = sb.freeClusterHint;
        if (c == 0) c = 1;
        
        uint64_t startC = c;
        bool wrapped = false;
        
        while (true) {
            c = skipPastReserved(c);
            
            if (c >= sb.totalClusters) {
                if (wrapped) return 0;
                wrapped = true;
                c = 1;
                continue;
            }
            
            if (wrapped && c >= startC) return 0;
            
            if (!isReservedCluster(c)) {
                LABEntry entry = getLABEntry(c);
                if (entry.nextCluster == LAT_FREE && entry.flags == 0) {
                    setLATEntryWithLevel(c, LAT_END, levelID);
                    updateLITAllocatedCount(c, 1);
                    sb.freeClusterHint = c + 1;
                    sb.totalFreeClusters--;
                    writeSuperBlock();
                    return c;
                }
            }
            
            c++;
        }
        return 0;
    }

    vector<uint64_t> getChain(uint64_t startCluster) {
        vector<uint64_t> chain;
        if (startCluster == 0 || startCluster >= sb.totalClusters) return chain;
        
        chain.push_back(startCluster);
        
        if (isReservedCluster(startCluster)) {
            return chain;
        }
        
        uint64_t current = getLATEntry(startCluster);
        while (current != 0 && current != LAT_END && current != LAT_BAD && current < sb.totalClusters) {
            if (find(chain.begin(), chain.end(), current) != chain.end()) break;
            chain.push_back(current);
            if (chain.size() > 1000000) break;
            current = getLATEntry(current);
        }
        return chain;
    }


    // Helper: Try loading backup superblock
    bool tryBackupSuperblock() {
        if (sb.backupSBCluster == 0) return false;
        
        SuperBlock backupSB;
        if (!disk.readSector(sb.backupSBCluster * 8, &backupSB)) return false;
        if (backupSB.magic != MAGIC) return false;
        
        lout << "[Recovery] Primary SuperBlock corrupt. Using backup from cluster " << sb.backupSBCluster << "\n";
        sb = backupSB;
        // Restore primary from backup
        disk.writeSector(0, &sb);
        return true;
    }
    
    // Helper: Write superblock to both locations
    void writeSuperBlock() {
        disk.writeSector(0, &sb);
        if (sb.backupSBCluster != 0) {
            disk.writeSector(sb.backupSBCluster * 8, &sb);
        }
    }

    bool isMounted() { return disk.isOpen(); }

    DiskDevice& getDiskDevice() { return disk; }
    SuperBlock& getSuperBlock() { return sb; }
    NavigationContext& getNavigationContext() { return context; }
    FileLockManager& getFileLockManager() { return lockManager; }
    ThreadPool& getThreadPool() { return threadPool; }
    TaskHandler& getTaskHandler() { return taskHandler; }
    
    void listBackgroundTasks() { taskHandler.listTasks(); }
    
    uint32_t runInBackground(const string& taskName, function<void()> func) {
        return taskHandler.runBackground(taskName, func);
    }


    bool loadVersion(const string& ver) {
        vector<uint64_t> chain = getChain(context.currentDirCluster);
        
        for (uint64_t c : chain) {
        char vpsBuf[SECTOR_SIZE];
        for (int i=0; i<8; i++) {
             disk.readSector(c * 8 + i, vpsBuf);
             VersionEntry* vps = (VersionEntry*)vpsBuf;
             for(int j=0; j<SECTOR_SIZE/sizeof(VersionEntry); j++) {
                    if (vps[j].isActive && string(vps[j].versionName) == ver) {
                        context.currentContentCluster = vps[j].contentTableCluster;
                        context.currentLevelID = vps[j].levelID;
                        context.currentVersion = ver;
                        return true;
                    }
                }
            }
        }
        return false;
    }

    struct PathResult {
        uint64_t parentCluster;
        string name;
        bool valid;
    };
    
    struct FileParts {
        string name;
        string ext;
    };
    
    FileParts parseFileName(const string& fullName) {
        FileParts parts;
        size_t dotPos = fullName.find_last_of('.');
        if (dotPos != string::npos && dotPos > 0) {
            parts.name = fullName.substr(0, dotPos);
            parts.ext = fullName.substr(dotPos + 1);
        } else {
            parts.name = fullName;
            parts.ext = "";
        }
        return parts;
    }
    
    bool matchesFile(const DirEntry& entry, const FileParts& fp) {
        if (entry.type != TYPE_FILE && entry.type != TYPE_SYMLINK && entry.type != TYPE_HARDLINK) return false;
        bool nameMatch = (string(entry.name) == fp.name);
        bool extMatch = fp.ext.empty() || (string(entry.extension) == fp.ext);
        return nameMatch && extMatch;
    }
    
    bool matchesEntry(const DirEntry& entry, const FileParts& fp) {
        if (entry.type == TYPE_FREE) return false;
        bool nameMatch = (string(entry.name) == fp.name);
        if (entry.type == TYPE_LEVELED_DIR || entry.type == TYPE_LEVEL_MOUNT) {
            return nameMatch && fp.ext.empty();
        }
        bool extMatch = fp.ext.empty() || (string(entry.extension) == fp.ext);
        return nameMatch && extMatch;
    }

    PathResult resolvePath(string path) {
        if (path.empty()) return {context.currentContentCluster, "", false};
        
        uint64_t current = context.currentContentCluster;
        if (path[0] == '/') {
            if (context.rootContentCluster == 0) return {0, "", false};
            current = context.rootContentCluster;
            path = path.substr(1);
        }
        if (path.empty()) return {current, "", true};

        vector<string> parts = PathUtils::splitPath(path);

        if (parts.empty()) return {current, "", true};

        for (size_t i = 0; i < parts.size() - 1; i++) {
            string folderName = parts[i];
            string levelName = "master";
            size_t colon = folderName.find(':');
            if (colon != string::npos) {
                levelName = folderName.substr(colon + 1);
                folderName = folderName.substr(0, colon);
            }

            DirEntry entries[SECTOR_SIZE/sizeof(DirEntry)];
            uint64_t folderCluster = 0;
            bool found = false;
            
            // Follow LAT chain for directory search
            vector<uint64_t> chain = getChain(current);
            for (uint64_t c : chain) {
                for (int s = 0; s < 8; s++) {
                    memset(entries, 0, sizeof(entries));
                    disk.readSector(c * 8 + s, entries);
                    for (int j = 0; j < SECTOR_SIZE/sizeof(DirEntry); j++) {
                        if (entries[j].type == TYPE_LEVELED_DIR && string(entries[j].name) == folderName) {
                            folderCluster = entries[j].startCluster;
                            found = true;
                            break;
                        }
                    }
                    if (found) break;
                }
                if (found) break;
            }


            if (!found) return {0, "", false};

            char vpsBuf[SECTOR_SIZE];
            VersionEntry* vps = (VersionEntry*)vpsBuf;
            bool levelFound = false;
            uint64_t nextContent = 0;
            for (int s = 0; s < 8; s++) {
                disk.readSector(folderCluster * 8 + s, vpsBuf);
                for (int j = 0; j < SECTOR_SIZE/sizeof(VersionEntry); j++) {
                    if (vps[j].isActive && string(vps[j].versionName) == levelName) {
                        nextContent = vps[j].contentTableCluster;
                        levelFound = true;
                        break;
                    }
                }
                if (levelFound) break;
            }

            if (!levelFound) return {0, "", false};
            current = nextContent;
        }

        return {current, parts.back(), true};
    }

    void look(string target = "") {
        if (!disk.isOpen()) return;
        
        if (target.empty() && !ctxManager->canRead()) {
            lout << "Permission denied: no read access to current folder.\n";
            return;
        }
        
        uint64_t contentCluster = context.currentContentCluster;
        string itemsTitle = context.currentPath + " (" + context.currentVersion + ")";

        if (!target.empty()) {
            PathResult res = resolvePath(target);
            if (!res.valid) {
                 lout << "Path not found.\n";
                 return;
            }
            string folderName = res.name;
            string levelName = "";
            size_t colon = folderName.find(':');
            if (colon != string::npos) {
                levelName = folderName.substr(colon + 1);
                folderName = folderName.substr(0, colon);
            }

             DirEntry entries[SECTOR_SIZE/sizeof(DirEntry)];
             uint64_t foundCluster = 0;
             uint8_t foundType = 0;
             bool found = false;

             // Follow LAT chain for target search
             vector<uint64_t> chain = getChain(res.parentCluster);
             for (uint64_t c : chain) {
                 for (int i=0; i<8; i++) {
                     memset(entries, 0, sizeof(entries));
                     disk.readSector(c * 8 + i, entries);
                     for (int j=0; j<SECTOR_SIZE/sizeof(DirEntry); j++) {
                         if (entries[j].type != TYPE_FREE && string(entries[j].name) == folderName) {
                             foundCluster = entries[j].startCluster;
                             foundType = entries[j].type;
                             found = true;
                             break;
                         }
                     }
                     if (found) break;
                 }
                 if (found) break;
             }

             
             if (!found) { lout << "Target not found.\n"; return; }
             
             if (foundType == TYPE_FILE) {
                 lout << "File: " << folderName << "\n";
                 return; 
             }
             
             if (levelName.empty()) {
                  lout << "Levels of " << folderName << ":\n";
                  char vpsBuf[SECTOR_SIZE];
                  VersionEntry* vps = (VersionEntry*)vpsBuf;
                  for (int i=0; i<8; i++) {
                      disk.readSector(foundCluster * 8 + i, vpsBuf);
                      for (int j=0; j<SECTOR_SIZE/sizeof(VersionEntry); j++) {
                          if (vps[j].isActive) lout << " [" << vps[j].versionName << "]\n";
                      }
                  }
                  return;
             } else {
                 char vpsBuf[SECTOR_SIZE];
                 VersionEntry* vps = (VersionEntry*)vpsBuf;
                 bool lvlFound = false;
                 for (int i=0; i<8; i++) {
                     disk.readSector(foundCluster * 8 + i, vpsBuf);
                     for (int j=0; j<SECTOR_SIZE/sizeof(VersionEntry); j++) {
                         if (vps[j].isActive && string(vps[j].versionName) == levelName) {
                             contentCluster = vps[j].contentTableCluster;
                             itemsTitle = folderName + ":" + levelName;
                             lvlFound = true;
                             break;
                         }
                     }
                     if (lvlFound) break;
                 }
                 if (!lvlFound) { lout << "Level '" << levelName << "' not found.\n"; return; }
             }
        }
        
        lout << "Content of " << itemsTitle << ":\n";
        DirEntry entries[SECTOR_SIZE/sizeof(DirEntry)];
        bool empty = true;
        
        // Follow LAT chain for directory contents
        vector<uint64_t> chain = getChain(contentCluster);
        for (uint64_t c : chain) {
            for (int i=0; i<8; i++) {
                memset(entries, 0, sizeof(entries));
                if (!disk.readSector(c * 8 + i, entries)) continue;
                for (int j=0; j<SECTOR_SIZE/sizeof(DirEntry); j++) {
                    if (entries[j].type == TYPE_FILE || entries[j].type == TYPE_LEVELED_DIR || 
                        entries[j].type == TYPE_SYMLINK || entries[j].type == TYPE_HARDLINK ||
                        entries[j].type == TYPE_LEVEL_MOUNT) {
                        empty = false;
                        entries[j].name[23] = '\0';
                        string typeStr;
                        if (entries[j].type == TYPE_LEVELED_DIR) typeStr = "<L-DIR>";
                        else if (entries[j].type == TYPE_FILE) typeStr = "<FILE>";
                        else if (entries[j].type == TYPE_SYMLINK) typeStr = "<SYMLNK>";
                        else if (entries[j].type == TYPE_HARDLINK) typeStr = "<HDLINK>";
                        else if (entries[j].type == TYPE_LEVEL_MOUNT) typeStr = "<LVLMNT>";
                        
                        entries[j].extension[3] = '\0';
                        string displayName = entries[j].name;
                        if (entries[j].type == TYPE_FILE && entries[j].extension[0] != '\0') {
                            displayName += ".";
                            displayName += entries[j].extension;
                        }
                        
                        lout << setw(8) << left << typeStr << " " << displayName;
                        
                        if (entries[j].type == TYPE_SYMLINK && entries[j].startCluster != 0) {
                            char targetBuf[CLUSTER_SIZE];
                            memset(targetBuf, 0, CLUSTER_SIZE);
                            for (int s=0; s<8; s++) {
                                disk.readSector(entries[j].startCluster * 8 + s, targetBuf + (s * SECTOR_SIZE));
                            }
                            lout << " -> " << targetBuf;
                        }
                        if (entries[j].type == TYPE_LEVEL_MOUNT) {
                            LevelDescriptor* lvl = findLevelByID(entries[j].startCluster);
                            if (lvl) lout << " -> Level '" << lvl->name << "' (ID: " << entries[j].startCluster << ")";
                            else lout << " -> Level ID: " << entries[j].startCluster;
                        }
                        lout << "\n";
                    }
                }
            }
        }
        if (empty) lout << "(empty)\n";
        lout.flush();
    }

    void lookTarget(string target) {
        if (!disk.isOpen()) return;
        
        string folderName = target;
        string levelName = "";
        size_t colon = target.find(':');
        if (colon != string::npos) {
            folderName = target.substr(0, colon);
            levelName = target.substr(colon + 1);
        }
        
        DirEntry entries[SECTOR_SIZE/sizeof(DirEntry)];
        uint64_t folderCluster = 0;
        for (int i=0; i<8; i++) {
            memset(entries, 0, sizeof(entries));
            disk.readSector(context.currentContentCluster * 8 + i, entries);
            for (int j=0; j<SECTOR_SIZE/sizeof(DirEntry); j++) {
                if (entries[j].type == TYPE_LEVELED_DIR && string(entries[j].name) == folderName) {
                    folderCluster = entries[j].startCluster;
                    break;
                }
            }
            if (folderCluster) break;
        }
        
        if (!folderCluster) {
            lout << "Folder '" << folderName << "' not found.\n";
            return;
        }
        
        if (levelName.empty()) {
            lout << "Levels of '" << folderName << "':\n";
            char vpsBuf[SECTOR_SIZE];
            VersionEntry* vps = (VersionEntry*)vpsBuf;
            int count = 0;
            for (int i=0; i<8; i++) {
                disk.readSector(folderCluster * 8 + i, vpsBuf);
                for (int j=0; j<SECTOR_SIZE/sizeof(VersionEntry); j++) {
                    if (vps[j].isActive) {
                        lout << "  [" << vps[j].versionName << "]\n";
                        count++;
                    }
                }
            }
            if (count == 0) lout << "  (no levels)\n";
        } else {
            char vpsBuf[SECTOR_SIZE];
            VersionEntry* vps = (VersionEntry*)vpsBuf;
            uint64_t contentCluster = 0;
            for (int i=0; i<8; i++) {
                disk.readSector(folderCluster * 8 + i, vpsBuf);
                for (int j=0; j<SECTOR_SIZE/sizeof(VersionEntry); j++) {
                    if (vps[j].isActive && string(vps[j].versionName) == levelName) {
                        contentCluster = vps[j].contentTableCluster;
                        break;
                    }
                }
                if (contentCluster) break;
            }
            
            if (!contentCluster) {
                lout << "Level '" << levelName << "' not found in '" << folderName << "'.\n";
                return;
            }
            
            lout << "Content of " << folderName << ":" << levelName << ":\n";
            bool empty = true;
            for (int i=0; i<8; i++) {
                memset(entries, 0, sizeof(entries));
                disk.readSector(contentCluster * 8 + i, entries);
                for (int j=0; j<SECTOR_SIZE/sizeof(DirEntry); j++) {
                    if (entries[j].type == TYPE_FILE || entries[j].type == TYPE_LEVELED_DIR) {
                        empty = false;
                        entries[j].name[31] = '\0';
                        string typeStr = (entries[j].type == TYPE_LEVELED_DIR) ? "<L-DIR>" : "<FILE>";
                        lout << setw(8) << left << typeStr << " " << entries[j].name << "\n";
                    }
                }
            }
            if (empty) lout << "(empty)\n";
        }
        lout.flush();
    }

    void dirTreeRecurse(uint64_t contentCluster, string prefix, bool isLast) {
        DirEntry entries[SECTOR_SIZE/sizeof(DirEntry)];
        vector<pair<string, uint64_t>> folders;
        vector<string> files;
        
        for (int i=0; i<8; i++) {
            memset(entries, 0, sizeof(entries));
            disk.readSector(contentCluster * 8 + i, entries);
            for (int j=0; j<SECTOR_SIZE/sizeof(DirEntry); j++) {
                if (entries[j].type == TYPE_LEVELED_DIR) {
                    entries[j].name[23] = '\0';
                    if (string(entries[j].name) == "." || string(entries[j].name) == "..") continue;
                if (entries[j].startCluster == 0) continue;
                folders.push_back({string(entries[j].name), entries[j].startCluster});
                } else if (entries[j].type == TYPE_FILE) {
                    entries[j].name[23] = '\0';
                    entries[j].extension[7] = '\0';
                    string displayName = entries[j].name;
                    if (entries[j].extension[0] != '\0') {
                        displayName += ".";
                        displayName += entries[j].extension;
                    }
                    files.push_back(displayName);
                }
            }
        }
        
        for (size_t i = 0; i < files.size(); i++) {
            bool last = (i == files.size() - 1) && folders.empty();
            lout << prefix << (last ? "└── " : "├── ") << files[i] << "\n";
        }
        
        for (size_t i = 0; i < folders.size(); i++) {
            bool last = (i == folders.size() - 1);
            lout << prefix << (last ? "└── " : "├── ") << "[" << folders[i].first << "]" << "\n";
            
            char vpsBuf[SECTOR_SIZE];
            for (int s=0; s<8; s++) {
                disk.readSector(folders[i].second * 8 + s, vpsBuf);
                VersionEntry* vps = (VersionEntry*)vpsBuf;
                for (int j=0; j<SECTOR_SIZE/sizeof(VersionEntry); j++) {
                    if (vps[j].isActive) {
                        string newPrefix = prefix + (last ? "    " : "│   ");
                        bool lastLevel = true;
                        for (int k=j+1; k<SECTOR_SIZE/sizeof(VersionEntry); k++) {
                            if (vps[k].isActive) { lastLevel = false; break; }
                        }
                        if (lastLevel) {
                            for (int ns=s+1; ns<8 && lastLevel; ns++) {
                                char vp2Buf[SECTOR_SIZE];
                                disk.readSector(folders[i].second * 8 + ns, vp2Buf);
                                VersionEntry* vp2 = (VersionEntry*)vp2Buf;
                                for (int k=0; k<SECTOR_SIZE/sizeof(VersionEntry); k++) {
                                    if (vp2[k].isActive) { lastLevel = false; break; }
                                }
                            }
                        }
                        lout << newPrefix << (lastLevel ? "└── " : "├── ") << ":" << vps[j].versionName << "\n";
                        dirTreeRecurse(vps[j].contentTableCluster, newPrefix + (lastLevel ? "    " : "│   "), lastLevel);
                    }
                }
            }
        }
    }

    void dirTree() {
        if (!disk.isOpen()) return;
        
        vector<pair<string, uint64_t>> rootLevels;
        char vpsBuf[SECTOR_SIZE];
        VersionEntry* vps = (VersionEntry*)vpsBuf;
        
        for (int s = 0; s < 8; s++) {
            disk.readSector(sb.rootDirCluster * 8 + s, vpsBuf);
            for (int j = 0; j < SECTOR_SIZE / sizeof(VersionEntry); j++) {
                if (vps[j].isActive) {
                    rootLevels.push_back({string(vps[j].versionName), vps[j].contentTableCluster});
                }
            }
        }
        
        if (rootLevels.empty()) {
            lout << "No root levels found.\n";
            return;
        }
        
        string selectedLevel;
        uint64_t selectedCluster;
        
        if (rootLevels.size() == 1) {
            selectedLevel = rootLevels[0].first;
            selectedCluster = rootLevels[0].second;
        } else {
            lout << "Root levels available: ";
            for (const auto& lv : rootLevels) lout << "[" << lv.first << "] ";
            lout << "\nSelect level for tree: ";
            cin >> selectedLevel;
            cin.ignore(1000, '\n');
            
            bool found = false;
            for (const auto& lv : rootLevels) {
                if (lv.first == selectedLevel) {
                    selectedCluster = lv.second;
                    found = true;
                    break;
                }
            }
            if (!found) {
                lout << "Level '" << selectedLevel << "' not found.\n";
                return;
            }
        }
        
        lout << "/" << " (" << selectedLevel << ")\n";
        dirTreeRecurse(selectedCluster, "", true);
        lout.flush();
    }

    void createSymlink(string linkPath, string targetPath) {
        if (!disk.isOpen()) return;
        
        if (linkPath.find(':') != string::npos) {
            lout << "Cannot create symlink inside a level. Use a folder path.\n";
            return;
        }
        
        if (!ctxManager->canWrite()) {
            lout << "Permission denied: current folder is read-only.\n";
            return;
        }
        
        PathResult res = resolvePath(linkPath);
        if (!res.valid) { lout << "Invalid link path.\n"; return; }
        
        createInCluster(res.parentCluster, "symlink", res.name);
        
        // Find the newly created symlink entry and set its target
        vector<uint64_t> chain = getChain(res.parentCluster);
        for (uint64_t c : chain) {
            for (int i=0; i<8; i++) {
                DirEntry entries[SECTOR_SIZE/sizeof(DirEntry)];
                disk.readSector(c * 8 + i, entries);
                for (int j=0; j<SECTOR_SIZE/sizeof(DirEntry); j++) {
                    if (entries[j].type == TYPE_SYMLINK && string(entries[j].name) == res.name) {
                        // Allocate cluster to store target path
                        uint64_t targetCluster = allocCluster();
                        if (targetCluster == 0) {
                            lout << "Disk full. Cannot create symlink.\n";
                            entries[j].type = TYPE_FREE;
                            disk.writeSector(c * 8 + i, entries);
                            return;
                        }
                        
                        // Write target path to cluster
                        char buffer[CLUSTER_SIZE];
                        memset(buffer, 0, CLUSTER_SIZE);
                        strncpy(buffer, targetPath.c_str(), CLUSTER_SIZE - 1);
                        for (int s=0; s<8; s++) {
                            disk.writeSector(targetCluster * 8 + s, buffer + (s * SECTOR_SIZE));
                        }
                        
                        entries[j].startCluster = targetCluster;
                        entries[j].size = targetPath.length();
                        disk.writeSector(c * 8 + i, entries);
                        lout << "Symlink '" << res.name << "' -> '" << targetPath << "' created.\n";
                        return;
                    }
                }
            }
        }
    }
    
    void createHardlink(string linkPath, string targetPath) {
        if (!disk.isOpen()) return;
        
        if (linkPath.find(':') != string::npos || targetPath.find(':') != string::npos) {
            lout << "Cannot create hardlink with level paths.\n";
            return;
        }
        
        if (!ctxManager->canWrite()) {
            lout << "Permission denied: current folder is read-only.\n";
            return;
        }
        
        PathResult targetRes = resolvePath(targetPath);
        if (!targetRes.valid) { lout << "Target file not found.\n"; return; }
        
        // Find target file entry
        DirEntry targetEntry;
        bool found = false;
        vector<uint64_t> targetChain = getChain(targetRes.parentCluster);
        for (uint64_t c : targetChain) {
            for (int i=0; i<8; i++) {
                DirEntry entries[SECTOR_SIZE/sizeof(DirEntry)];
                disk.readSector(c * 8 + i, entries);
                for (int j=0; j<SECTOR_SIZE/sizeof(DirEntry); j++) {
                    if (entries[j].type == TYPE_FILE && string(entries[j].name) == targetRes.name) {
                        targetEntry = entries[j];
                        found = true;
                        
                        // Increment reference count
                        entries[j].attributes++;
                        disk.writeSector(c * 8 + i, entries);
                        goto found_target;
                    }
                }
            }
        }
found_target:
        if (!found) { lout << "Target must be a regular file.\n"; return; }
        
        // Create hardlink entry
        PathResult linkRes = resolvePath(linkPath);
        if (!linkRes.valid) { lout << "Invalid link path.\n"; return; }
        
        createInCluster(linkRes.parentCluster, "hardlink", linkRes.name);
        
        // Update the hardlink to point to same data cluster
        vector<uint64_t> linkChain = getChain(linkRes.parentCluster);
        for (uint64_t c : linkChain) {
            for (int i=0; i<8; i++) {
                DirEntry entries[SECTOR_SIZE/sizeof(DirEntry)];
                disk.readSector(c * 8 + i, entries);
                for (int j=0; j<SECTOR_SIZE/sizeof(DirEntry); j++) {
                    if (entries[j].type == TYPE_HARDLINK && string(entries[j].name) == linkRes.name) {
                        entries[j].startCluster = targetEntry.startCluster;
                        entries[j].size = targetEntry.size;
                        entries[j].attributes = targetEntry.attributes;  // Share refcount
                        disk.writeSector(c * 8 + i, entries);
                        lout << "Hardlink '" << linkRes.name << "' -> '" << targetPath << "' created.\n";
                        return;
                    }
                }
            }
        }
    }
    
    void create(string type, string path, string extension = "") {
        if (!disk.isOpen()) return;
        
        if (!ctxManager->canWrite()) {
            lout << "Permission denied: current folder is read-only.\n";
            return;
        }
        
        PathResult res = resolvePath(path);
        if (!res.valid) { lout << "Invalid path location.\n"; return; }
        
        createInCluster(res.parentCluster, type, res.name, extension);
    }
    
    void createInCluster(uint64_t contentCluster, string type, string name, string extension = "") {
        if (!PathUtils::isValidName(name)) {
            lout << "Invalid name: '" << name << "'. Names must be 1-23 chars without / \\ : * ? \" < > |\n";
            return;
        }
        if (!extension.empty() && !PathUtils::isValidExtension(extension)) {
            lout << "Invalid extension: '" << extension << "'. Extensions must be 1-7 chars.\n";
            return;
        }
        
        DirEntry entries[SECTOR_SIZE/sizeof(DirEntry)];
        int freeSector = -1;
        int freeIdx = -1;
        uint64_t targetCluster = contentCluster;
        
        vector<uint64_t> chain = getChain(contentCluster);
        
        if (chain.empty()) {
            lout << "Error: Invalid directory cluster. Cannot create.\n";
            return;
        }
        
        bool slotFound = false;
        
        for (uint64_t c : chain) {
            for (int i=0; i<8; i++) {
                disk.readSector(c * 8 + i, entries);
                for (int j=0; j<SECTOR_SIZE/sizeof(DirEntry); j++) {
                    if (entries[j].type == TYPE_FREE) {
                        freeSector = i;
                        freeIdx = j;
                        targetCluster = c;
                        slotFound = true;
                        goto found_slot;
                    }
                }
            }
        }

found_slot:
        if (!slotFound) {
            uint64_t lastCluster = chain.back();
            uint64_t newCluster = allocCluster();
            if (newCluster == 0) {
                lout << "Disk full. Cannot create " << type << ".\n";
                return;
            }
            
            setLATEntry(lastCluster, newCluster);
            
            memset(entries, 0, sizeof(entries));
            for (int i=0; i<8; i++) {
                disk.writeSector(newCluster * 8 + i, entries);
            }
            
            targetCluster = newCluster;
            freeSector = 0;
            freeIdx = 0;
            
            disk.readSector(newCluster * 8, entries);
        }
        
        DirEntry* target = &entries[freeIdx];
        memset(target, 0, sizeof(DirEntry));
        string truncName = PathUtils::truncateName(name, 23);
        string truncExt = PathUtils::truncateName(extension, 3);
        strncpy(target->name, truncName.c_str(), 23);
        target->name[23] = '\0';
        strncpy(target->extension, truncExt.c_str(), 3);
        target->extension[3] = '\0';
        target->createTime = time(0);
        target->modTime = time(0);
        target->accessTime = time(0);
        target->ownerLevel = static_cast<uint8_t>(context.currentLevelID & 0xFF);
        
        if (type == "folder") {
            target->type = TYPE_LEVELED_DIR;
            target->startCluster = allocCluster();
            if (target->startCluster == 0) {
                lout << "Disk full. Cannot create folder version table.\n";
                target->type = TYPE_FREE; // Mark entry as free again
                disk.writeSector(targetCluster * 8 + freeSector, entries); // Write back to disk
                return;
            }
            target->attributes = PERM_DIR_DEFAULT;
            
            if (target->startCluster != 0) {
                char vTableBuf[CLUSTER_SIZE];
                VersionEntry* vTable = (VersionEntry*)vTableBuf;
                memset(vTableBuf, 0, sizeof(vTableBuf));
                strcpy(vTable[0].versionName, "master");
                vTable[0].isActive = 1;
                vTable[0].contentTableCluster = allocCluster();
                if (vTable[0].contentTableCluster == 0) {
                    lout << "Disk full. Cannot create folder content table.\n";
                    // Need to free previously allocated cluster for version table
                    setLATEntry(target->startCluster, LAT_FREE);
                    target->type = TYPE_FREE; // Mark entry as free again
                    disk.writeSector(targetCluster * 8 + freeSector, entries); // Write back to disk
                    return;
                }
                vTable[0].levelID = context.currentLevelID;
                vTable[0].parentLevelID = context.rootLevelID;
                vTable[0].flags = LEVEL_FLAG_ACTIVE;
                vTable[0].permissions = PERM_DIR_DEFAULT;
                vTable[0].createTime = time(0);
                vTable[0].modTime = time(0);
                vTable[0].isLocked = 0;
                vTable[0].isSnapshot = 0;
                
                for (int s = 0; s < 8; s++) {
                    disk.writeSector(target->startCluster * 8 + s, vTableBuf + s * SECTOR_SIZE);
                }
                
                if (vTable[0].contentTableCluster != 0) {
                    DirEntry emptyContent[CLUSTER_SIZE / sizeof(DirEntry)];
                    memset(emptyContent, 0, sizeof(emptyContent));
                    for (int s = 0; s < 8; s++) {
                        disk.writeSector(vTable[0].contentTableCluster * 8 + s, ((char*)emptyContent) + s * SECTOR_SIZE);
                    }
                }
            }
        } else if (type == "symlink") {
            target->type = TYPE_SYMLINK;
            target->startCluster = 0;
            target->size = 0;
            target->attributes = PERM_DEFAULT;
        } else if (type == "hardlink") {
            target->type = TYPE_HARDLINK;
            target->startCluster = 0;
            target->size = 0;
            target->attributes = PERM_DEFAULT;
        } else {
            target->type = TYPE_FILE;
            target->startCluster = allocCluster();
            if (target->startCluster == 0) {
                lout << "Disk full. Cannot create file content.\n";
                target->type = TYPE_FREE; // Mark entry as free again
                disk.writeSector(targetCluster * 8 + freeSector, entries); // Write back to disk
                return;
            }
            target->size = 0;
            target->attributes = PERM_DEFAULT;
        }
        
        disk.writeSector(targetCluster * 8 + freeSector, entries);
        
        updateLevelStats(context.currentLevelID, target->size, 1);
        
        string displayName = name;
        if (!extension.empty()) displayName += "." + extension;
        lout << "Created " << type << " " << displayName << " [" << getPermsStr(target->attributes) << "]\n";
    }
    
    string getPermsStr(uint32_t attrs) {
        string s;
        s += (attrs & PERM_READ) ? 'r' : '-';
        s += (attrs & PERM_WRITE) ? 'w' : '-';
        s += (attrs & PERM_EXEC) ? 'x' : '-';
        return s;
    }
    
    string formatTime(uint32_t t) {
        if (t == 0) return "----";
        time_t tt = t;
        struct tm* tm = localtime(&tt);
        char buf[20];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", tm);
        return string(buf);
    }
    
    bool checkCurrentDirWrite() {
        if (context.currentPath == "/") return true;
        return PermissionChecker::checkWrite(context.currentFolderPerms);
    }
    
    bool checkCurrentDirRead() {
        if (context.currentPath == "/") return true;
        return PermissionChecker::checkRead(context.currentFolderPerms);
    }
    
    bool checkCurrentDirExec() {
        if (context.currentPath == "/") return true;
        return PermissionChecker::checkExec(context.currentFolderPerms);
    }
    
    uint32_t getEntryPermsFromDisk(uint64_t cluster, const string& name) {
        if (!permResolver) return PERM_DEFAULT;
        PermissionResult result = permResolver->readEntryPerms(cluster, name, nullptr);
        if (result.found) {
            return result.perms;
        }
        return PERM_DEFAULT;
    }
    
    void updateAccessTime(uint64_t parentCluster, const string& name) {
        vector<uint64_t> chain = getChain(parentCluster);
        for (uint64_t c : chain) {
            for (int i = 0; i < 8; i++) {
                DirEntry entries[SECTOR_SIZE / sizeof(DirEntry)];
                disk.readSector(c * 8 + i, entries);
                for (int j = 0; j < SECTOR_SIZE / sizeof(DirEntry); j++) {
                    if (entries[j].type != TYPE_FREE && string(entries[j].name) == name) {
                        entries[j].accessTime = time(0);
                        disk.writeSector(c * 8 + i, entries);
                        return;
                    }
                }
            }
        }
    }
    
    void setLevelPerms(string options, string folderName, string levelName) {
        uint64_t folderCluster = findFolderCluster(folderName);
        if (folderCluster == 0) {
            lout << "Folder '" << folderName << "' not found.\n";
            return;
        }
        
        char vpsBuf[SECTOR_SIZE];
        VersionEntry* vps = (VersionEntry*)vpsBuf;
        
        for (int s = 0; s < 8; s++) {
            disk.readSector(folderCluster * 8 + s, vpsBuf);
            for (int j = 0; j < SECTOR_SIZE / sizeof(VersionEntry); j++) {
                if (vps[j].isActive && string(vps[j].versionName) == levelName) {
                    if (options == "+r") vps[j].permissions |= PERM_READ;
                    else if (options == "-r") vps[j].permissions &= ~PERM_READ;
                    else if (options == "+w") vps[j].permissions |= PERM_WRITE;
                    else if (options == "-w") vps[j].permissions &= ~PERM_WRITE;
                    else if (options == "+x") vps[j].permissions |= PERM_EXEC;
                    else if (options == "-x") vps[j].permissions &= ~PERM_EXEC;
                    else { lout << "Invalid option. Use +r,-r,+w,-w,+x,-x\n"; return; }
                    
                    vps[j].modTime = time(0);
                    disk.writeSector(folderCluster * 8 + s, vpsBuf);
                    permCache.clear();
                    lout << "Level '" << levelName << "' permissions: " 
                         << PermissionChecker::getPermsString(vps[j].permissions) << "\n";
                    return;
                }
            }
        }
        lout << "Level '" << levelName << "' not found in '" << folderName << "'.\n";
    }
    
    void perms(string options, string path) {
        if (!disk.isOpen()) return;
        
        size_t colonPos = path.find(':');
        if (colonPos != string::npos && colonPos > 0) {
            string folderName = path.substr(0, colonPos);
            string levelName = path.substr(colonPos + 1);
            setLevelPerms(options, folderName, levelName);
            return;
        }
        
        PathResult res = resolvePath(path);
        if (!res.valid) { lout << "Item not found.\n"; return; }
        
        if (!PermissionChecker::isValidOption(options)) {
            lout << "Invalid option. Use +r,-r,+w,-w,+x,-x,+h,-h,+s,-s\n";
            return;
        }
        
        if (!permResolver) {
            lout << "Permission resolver not initialized.\n";
            return;
        }
        
        PermissionResult current = permResolver->readEntryPerms(res.parentCluster, res.name, nullptr);
        if (!current.found) {
            lout << "File not found.\n";
            return;
        }
        
        uint32_t newPerms = PermissionChecker::parsePermString(options, current.perms);
        PermissionResult result = permResolver->writeEntryPerms(res.parentCluster, res.name, newPerms);
        
        if (result.found) {
            lout << "Permissions: " << PermissionChecker::getPermsString(result.perms) << "\n";
        } else {
            lout << "Failed to update permissions: " << result.errorMessage << "\n";
        }
    }
    
    void lookDetailed(string path = "") {
        if (!disk.isOpen()) return;
        
        uint64_t contentCluster = context.currentContentCluster;
        string title = context.currentPath;
        
        if (!path.empty()) {
            PathResult res = resolvePath(path);
            if (!res.valid) { lout << "Invalid path.\n"; return; }
            
            // Find the folder
            DirEntry entries[SECTOR_SIZE/sizeof(DirEntry)];
            vector<uint64_t> chain = getChain(res.parentCluster);
            for (uint64_t c : chain) {
                for (int i = 0; i < 8; i++) {
                    disk.readSector(c * 8 + i, entries);
                    for (int j = 0; j < SECTOR_SIZE/sizeof(DirEntry); j++) {
                        if (entries[j].type == TYPE_LEVELED_DIR && string(entries[j].name) == res.name) {
                            // Need to pick a level - use first active
                            char vpsBuf[SECTOR_SIZE];
                            VersionEntry* vps = (VersionEntry*)vpsBuf;
                            disk.readSector(entries[j].startCluster * 8, vpsBuf);
                            for (int v = 0; v < SECTOR_SIZE/sizeof(VersionEntry); v++) {
                                if (vps[v].isActive) {
                                    contentCluster = vps[v].contentTableCluster;
                                    title = path + ":" + vps[v].versionName;
                                    goto found;
                                }
                            }
                        }
                    }
                }
            }
            lout << "Folder not found.\n";
            return;
        }
        
found:
        lout << "\n" << title << " (detailed):\n";
        lout << string(90, '-') << "\n";
        lout << setw(8) << left << "Type" << " " << setw(5) << "Perms" << " " 
             << setw(10) << right << "Size" << "  " << setw(16) << left << "Modified" << "  "
             << setw(16) << left << "Accessed" << "  " << setw(3) << "Lvl" << "  Name\n";
        lout << string(90, '-') << "\n";
        
        string permsStr = getPermsStr(context.currentFolderPerms);
        string nowStr = formatTime((uint32_t)time(nullptr));
        
        lout << setw(8) << left << "<DIR>" << " " 
             << setw(5) << permsStr << " "
             << setw(10) << right << "-" << "  "
             << setw(16) << left << nowStr << "  "
             << setw(16) << left << "-" << "  "
             << setw(3) << "-" << "  .\n";
        
        lout << setw(8) << left << "<DIR>" << " " 
             << setw(5) << "rwx" << " "
             << setw(10) << right << "-" << "  "
             << setw(16) << left << nowStr << "  "
             << setw(16) << left << "-" << "  "
             << setw(3) << "-" << "  ..\n";
        
        DirEntry entries[SECTOR_SIZE/sizeof(DirEntry)];
        bool empty = true;
        
        vector<uint64_t> chain = getChain(contentCluster);
        for (uint64_t c : chain) {
            for (int i = 0; i < 8; i++) {
                disk.readSector(c * 8 + i, entries);
                for (int j = 0; j < SECTOR_SIZE/sizeof(DirEntry); j++) {
                    if (entries[j].type != TYPE_FREE && entries[j].name[0] != '\0') {
                        empty = false;
                        entries[j].name[23] = '\0';
                        entries[j].extension[3] = '\0';
                        
                        string typeStr;
                        if (entries[j].type == TYPE_LEVELED_DIR) typeStr = "<DIR>";
                        else if (entries[j].type == TYPE_FILE) typeStr = "<FILE>";
                        else if (entries[j].type == TYPE_SYMLINK) typeStr = "<LINK>";
                        else if (entries[j].type == TYPE_HARDLINK) typeStr = "<HARD>";
                        else if (entries[j].type == TYPE_LEVEL_MOUNT) typeStr = "<MNT>";
                        else typeStr = "<?>";
                        
                        string displayName = entries[j].name;
                        if (entries[j].type == TYPE_FILE && entries[j].extension[0] != '\0') {
                            displayName += ".";
                            displayName += entries[j].extension;
                        }
                        
                        string perms = getPermsStr(entries[j].attributes);
                        string sizeStr = (entries[j].type == TYPE_FILE) ? to_string(entries[j].size) : "-";
                        string modStr = formatTime(entries[j].modTime);
                        string accessStr = formatTime(entries[j].accessTime);
                        string levelStr = to_string(entries[j].ownerLevel);
                        
                        lout << setw(8) << left << typeStr << " " 
                             << setw(5) << perms << " "
                             << setw(10) << right << sizeStr << "  "
                             << setw(16) << left << modStr << "  "
                             << setw(16) << left << accessStr << "  "
                             << setw(3) << levelStr << "  "
                             << displayName << "\n";
                    }
                }
            }
        }
        if (empty) lout << "(empty)\n";
        lout << string(90, '-') << "\n";
    }
    
    // Filesystem check - validates integrity
    void fsck() {
        if (!disk.isOpen()) return;
        
        lout << "\n=== LevelFS Filesystem Check ===\n\n";
        
        int errors = 0;
        int warnings = 0;
        
        // Check 1: SuperBlock integrity
        lout << "[1/5] Checking SuperBlock...\n";
        if (sb.magic != MAGIC) {
            lout << "  ERROR: Invalid magic number!\n";
            errors++;
        } else {
            lout << "  OK: Magic number valid.\n";
        }
        
        if (sb.totalClusters == 0 || sb.totalClusters > 0xFFFFFFFF) {
            lout << "  ERROR: Invalid cluster count!\n";
            errors++;
        } else {
            lout << "  OK: Cluster count valid (" << sb.totalClusters << ").\n";
        }
        
        // Check 2: Root directory
        lout << "[2/5] Checking root directory...\n";
        if (sb.rootDirCluster == 0 || sb.rootDirCluster >= sb.totalClusters) {
            lout << "  ERROR: Invalid root directory cluster!\n";
            errors++;
        } else {
            uint8_t testBuf[SECTOR_SIZE];
            if (!disk.readSector(sb.rootDirCluster * 8, testBuf)) {
                lout << "  ERROR: Cannot read root directory!\n";
                errors++;
            } else {
                lout << "  OK: Root directory readable.\n";
            }
        }
        
        // Check 3: Level registry
        lout << "[3/5] Checking level registry...\n";
        if (sb.levelRegistryCluster == 0) {
            lout << "  WARNING: No level registry.\n";
            warnings++;
        } else {
            int levelCount = 0;
            vector<uint64_t> chain = getChain(sb.levelRegistryCluster);
            for (uint64_t c : chain) {
                char regBuf[SECTOR_SIZE];
                LevelDescriptor* reg = (LevelDescriptor*)regBuf;
                for (int s = 0; s < 8; s++) {
                    disk.readSector(c * 8 + s, regBuf); // Read into buffer
                    for (int j = 0; j < SECTOR_SIZE / sizeof(LevelDescriptor); j++) {
                        if (reg[j].flags & LEVEL_FLAG_ACTIVE) levelCount++;
                    }
                }
            }
            lout << "  OK: " << levelCount << " active levels found.\n";
        }
        
        // Check 4: Free space consistency 
        lout << "[4/5] Checking free space ... ";
        lout.flush();
        
        uint64_t reportedFree = sb.totalFreeClusters;
        atomic<uint64_t> sampleFree(0);
        atomic<uint64_t> sampleCount(0);
        
        uint64_t dataStart = sb.labPoolStart + sb.labPoolClusters;
        uint64_t totalToCheck = min(sb.totalClusters - dataStart, (uint64_t)100000);
        
        size_t numWorkers = threadPool.getWorkerCount();
        size_t samplesPerWorker = (totalToCheck / 100 + numWorkers - 1) / numWorkers;
        
        vector<future<pair<uint64_t, uint64_t>>> futures;
        
        for (size_t w = 0; w < numWorkers; w++) {
            size_t startSample = w * samplesPerWorker;
            size_t endSample = min(startSample + samplesPerWorker, totalToCheck / 100);
            
            futures.push_back(threadPool.enqueue([this, dataStart, startSample, endSample]() {
                uint64_t localFree = 0;
                uint64_t localCount = 0;
                for (size_t i = startSample; i < endSample; i++) {
                    uint64_t cluster = dataStart + i * 100;
                    if (cluster >= sb.totalClusters) break;
                    LABEntry lab = getLABEntry(cluster);
                    if (lab.nextCluster == LAT_FREE) localFree++;
                    localCount++;
                }
                return make_pair(localFree, localCount);
            }));
        }
        
        for (auto& f : futures) {
            auto result = f.get();
            sampleFree += result.first;
            sampleCount += result.second;
        }
        
        lout << "done\n";
        
        if (sampleCount > 0 && sampleFree > 0) {
            uint64_t estimatedFree = (sampleFree * 100);
            lout << "  OK: ~" << estimatedFree << " free clusters (parallel sampled).\n";
        } else if (reportedFree > 0) {
            lout << "  OK: " << reportedFree << " free clusters (from superblock).\n";
        } else {
            lout << "  WARNING: Disk may be full.\n";
            warnings++;
        }
        
        // Check 5: Journal
        lout << "[5/5] Checking journal...\n";
        if (sb.journalStartCluster == 0) {
            lout << "  WARNING: No journal configured.\n";
            warnings++;
        } else {
            lout << "  OK: Journal at cluster " << sb.journalStartCluster << ".\n";
        }
        
        lout << "\n=== Check Complete ===\n";
        lout << "Errors: " << errors << ", Warnings: " << warnings << "\n";
        if (errors == 0) {
            lout << "Filesystem appears healthy.\n";
        } else {
            lout << "Filesystem has errors. Consider reformatting.\n";
        }
    }
    
    // Analyze fragmentation using Defragmenter class
    void fragInfo() {
        if (!disk.isOpen()) return;
        Defragmenter defragmenter(disk, sb);
        defragmenter.analyze(context.currentContentCluster);
    }
    
    // Defragment disk using Defragmenter class
    void defrag(int flags = DEFRAG_NONE) {
        if (!disk.isOpen()) return;
        Defragmenter defragmenter(disk, sb);
        defragmenter.run(context.currentContentCluster, flags, context.currentPath);
    }
    
    // Defrag with string flags from command line
    void defragWithFlags(const string& flagStr) {
        int flags = DEFRAG_NONE;
        if (flagStr.find("-n") != string::npos || flagStr.find("--dry-run") != string::npos) {
            flags |= DEFRAG_DRY_RUN;
        }
        if (flagStr.find("-v") != string::npos || flagStr.find("--verbose") != string::npos) {
            flags |= DEFRAG_VERBOSE;
        }
        if (flagStr.find("-f") != string::npos || flagStr.find("--force") != string::npos) {
            flags |= DEFRAG_FORCE;
        }
        if (flagStr.find("-r") != string::npos || flagStr.find("--recursive") != string::npos) {
            flags |= DEFRAG_RECURSIVE;
        }
        defrag(flags);
    }
    
    void createLevelMount(string path, uint64_t levelID) {
        if (!disk.isOpen()) return;
        
        LevelDescriptor* level = findLevelByID(levelID);
        if (!level) {
            lout << "Level ID " << levelID << " not found.\n";
            return;
        }
        
        PathResult res = resolvePath(path);
        if (!res.valid) { lout << "Invalid path.\n"; return; }
        
        DirEntry entries[SECTOR_SIZE/sizeof(DirEntry)];
        vector<uint64_t> chain = getChain(res.parentCluster);
        
        for (uint64_t c : chain) {
            for (int i = 0; i < 8; i++) {
                disk.readSector(c * 8 + i, entries);
                for (int j = 0; j < SECTOR_SIZE/sizeof(DirEntry); j++) {
                    if (entries[j].type == TYPE_FREE) {
                        strcpy(entries[j].name, res.name.c_str());
                        entries[j].type = TYPE_LEVEL_MOUNT;
                        entries[j].startCluster = levelID;
                        entries[j].size = 0;
                        entries[j].createTime = time(0);
                        entries[j].modTime = time(0);
                        disk.writeSector(c * 8 + i, entries);
                        
                        level->refCount++;
                        updateLevelDescriptor(*level);
                        
                        lout << "Mounted level '" << level->name << "' (ID: " << levelID 
                             << ") at '" << res.name << "'\n";
                        return;
                    }
                }
            }
        }
        lout << "No space to create mount point.\n";
    }
    
    void updateLevelDescriptor(LevelDescriptor& updated) {
        vector<uint64_t> chain = getChain(sb.levelRegistryCluster);
        for (uint64_t c : chain) {
            char registryBuf[CLUSTER_SIZE];
            LevelDescriptor* registry = (LevelDescriptor*)registryBuf;
            for (int s = 0; s < SECTORS_PER_CLUSTER; s++) {
                 disk.readSector(c * SECTORS_PER_CLUSTER + s, registryBuf + s * SECTOR_SIZE);
            }
            for (int j = 0; j < CLUSTER_SIZE / sizeof(LevelDescriptor); j++) {
                if (registry[j].levelID == updated.levelID) {
                    registry[j] = updated;
                    for (int s = 0; s < SECTORS_PER_CLUSTER; s++) {
                        disk.writeSector(c * SECTORS_PER_CLUSTER + s,
                            registryBuf + s * SECTOR_SIZE);
                    }
                    return;
                }
            }
        }
    }

    
    void nav(string path) {
        if (!disk.isOpen()) return;
        if (path == ".") return;
        if (path == "..") {
            context.currentDirCluster = sb.rootDirCluster;
            context.currentPath = "/";
            context.currentFolderPerms = PERM_READ | PERM_WRITE | PERM_EXEC;
            loadVersion(context.rootVersion.empty() ? "master" : context.rootVersion);
            return;
        }
        string folderName = path;
        string levelName = "";
        size_t colon = path.find(':');
        if (colon != string::npos) {
            folderName = path.substr(0, colon);
            levelName = path.substr(colon+1);
            
            if (folderName == ".") {
                if (context.currentPath == "/") {
                    if (loadVersion(levelName)) {
                        context.rootVersion = levelName;
                        lout << "Switched to root level '" << levelName << "'\n";
                    } else {
                        lout << "Level '" << levelName << "' not found.\n";
                    }
                } else {
                    if (loadVersion(levelName)) {
                        lout << "Switched to level '" << levelName << "'\n";
                    } else {
                        lout << "Level '" << levelName << "' not found.\n";
                    }
                }
                return;
            }
        } else if (path.rfind(":", 0) == 0) {
             if (switchLevel(path.substr(1))) lout << "Switched to " << path.substr(1) << endl;
             else lout << "Version not found.\n";
             return;
        }

        DirEntry entries[SECTOR_SIZE/sizeof(DirEntry)];
        
        vector<uint64_t> chain = getChain(context.currentContentCluster);
        for (uint64_t c : chain) {
            for (int i=0; i<8; i++) {
                disk.readSector(c * 8 + i, entries);
                for (int j=0; j<SECTOR_SIZE/sizeof(DirEntry); j++) {
                    if (entries[j].type == TYPE_LEVELED_DIR && string(entries[j].name) == folderName) {
                        // Check permissions before entering
                        if (!(entries[j].attributes & PERM_READ)) {
                            lout << "Permission denied: no read access to '" << folderName << "'.\n";
                            return;
                        }
                        if (!(entries[j].attributes & PERM_EXEC)) {
                            lout << "Permission denied: no execute access to enter '" << folderName << "'.\n";
                            return;
                        }
                        context.currentFolderPerms = entries[j].attributes;
                        enterFolder(entries[j].startCluster, folderName, levelName);
                        return;
                    }
                    if (entries[j].type == TYPE_LEVEL_MOUNT && string(entries[j].name) == folderName) {
                        uint64_t mountedLevelID = entries[j].startCluster;
                        LevelDescriptor* level = findLevelByID(mountedLevelID);
                        if (level) {
                            ctxManager->switchVersion(level->name, level->rootContentCluster, level->levelID);
                            context.currentPath += folderName + "/";
                            lout << "Entered level mount '" << folderName << "' -> Level '" 
                                 << level->name << "' (ID: " << level->levelID << ")\n";
                        } else {
                            lout << "Mounted level not found.\n";
                        }
                        return;
                    }
                }
            }
        }
        lout << "Folder not found.\n";
    }
    
    void enterFolder(uint64_t cluster, string name, string level) {
    char vpsBuf[SECTOR_SIZE];
    vector<string> versions;
    for (int i=0; i<8; i++) {
         disk.readSector(cluster * 8 + i, vpsBuf);
         VersionEntry* vps = (VersionEntry*)vpsBuf;
         for(int j=0; j<SECTOR_SIZE/sizeof(VersionEntry); j++) {
             if (vps[j].isActive) versions.push_back(vps[j].versionName);
         }
    }
        if (versions.empty()) {
            lout << "Folder " << name << " has no versions.\n";
            lout << "Create default 'main'? (y/n): ";
            char ans; cin >> ans;
            if (ans == 'y') {
                addLevel(cluster, "main");
                level = "main";
            } else return;
        }
        if (level.empty()) {
            lout << "Available versions: ";
            for (const auto& v : versions) lout << "[" << v << "] ";
            lout << "\nSelect version: ";
            cin >> level;
        }
    // Find content
    uint64_t newContent = 0;
    bool found = false;
    for (int i=0; i<8; i++) {
         disk.readSector(cluster * 8 + i, vpsBuf);
         VersionEntry* vps = (VersionEntry*)vpsBuf;
         for(int j=0; j<SECTOR_SIZE/sizeof(VersionEntry); j++) {
             if (vps[j].isActive && string(vps[j].versionName) == level) {
                 newContent = vps[j].contentTableCluster;
                 found = true;
                 break;
             }
         }
         if(found) break;
    }    
        if (!found) { lout << "Version not found.\n"; return; }
        context.currentDirCluster = cluster;
        context.currentContentCluster = newContent;
        context.currentVersion = level;
        context.currentPath += name + "/";
    }
    
    bool switchLevel(string ver) { return loadVersion(ver); }
    
    uint64_t registerNewLevel(const string& name, uint64_t parentLevelID, uint64_t contentCluster) {
        if (sb.levelRegistryCluster == 0) return 0;
        
        uint64_t newLevelID = sb.nextLevelID++;
        sb.totalLevels++;
        
        SYSTEMTIME st;
        GetSystemTime(&st);
        FILETIME ft;
        SystemTimeToFileTime(&st, &ft);
        uint64_t timestamp = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        
        vector<uint64_t> chain = getChain(sb.levelRegistryCluster);
        
        for (uint64_t c : chain) {
            char registryBuf[SECTOR_SIZE];
            LevelDescriptor* registry = (LevelDescriptor*)registryBuf;
            for (int s = 0; s < 8; s++) {
                disk.readSector(c * 8 + s, registryBuf);
                for (int j = 0; j < SECTOR_SIZE / sizeof(LevelDescriptor); j++) {
                    if (registry[j].levelID == 0 && !(registry[j].flags & LEVEL_FLAG_ACTIVE)) {
                        strcpy(registry[j].name, name.c_str());
                        registry[j].levelID = newLevelID;
                        registry[j].parentLevelID = parentLevelID;
                        registry[j].rootContentCluster = contentCluster;
                        registry[j].createTime = timestamp;
                        registry[j].modTime = timestamp;
                        registry[j].flags = LEVEL_FLAG_ACTIVE;
                        registry[j].refCount = 1;
                        registry[j].childCount = 0;
                        disk.writeSector(c * 8 + s, registryBuf);
                        
                        writeSuperBlock();
                        return newLevelID;
                    }
                }
            }
        }
        
        return 0;
    }
    
    void addLevel(uint64_t cluster, string name) {
        uint64_t cont = allocCluster();
        if (cont == 0) {
            lout << "Disk full. Cannot add level.\n";
            return;
        }
        
        uint64_t newLevelID = registerNewLevel(name, context.currentLevelID, cont);
        
        vector<uint64_t> chain = getChain(cluster);
        
        for (uint64_t c : chain) {
            char vpsBuf[SECTOR_SIZE];
            VersionEntry* vps = (VersionEntry*)vpsBuf;
            for (int i=0; i<8; i++) {
                 disk.readSector(c * 8 + i, vpsBuf);
                 for(int j=0; j<SECTOR_SIZE/sizeof(VersionEntry); j++) {
                     if (!vps[j].isActive) {
                         strcpy(vps[j].versionName, name.c_str());
                         vps[j].contentTableCluster = cont;
                         vps[j].isActive = 1;
                         vps[j].levelID = newLevelID;
                         vps[j].parentLevelID = context.currentLevelID;
                         vps[j].flags = LEVEL_FLAG_ACTIVE;
                         disk.writeSector(c * 8 + i, vpsBuf);
                         lout << "Added level '" << name << "' (ID: " << newLevelID << ", Parent: " << context.currentLevelID << ")\n";
                         return;
                     }
                 }
            }
        }
        
        uint64_t lastCluster = chain.back();
        uint64_t newCluster = allocCluster();
        if (newCluster == 0) {
            lout << "Disk full. Cannot add level.\n";
            return;
        }
        
        setLATEntry(lastCluster, newCluster);
        
        char vpsBuf[SECTOR_SIZE];
        VersionEntry* vps = (VersionEntry*)vpsBuf;
        memset(vpsBuf, 0, sizeof(vpsBuf));
        strcpy(vps[0].versionName, name.c_str());
        vps[0].contentTableCluster = cont;
        vps[0].isActive = 1;
        vps[0].levelID = newLevelID;
        vps[0].parentLevelID = context.currentLevelID;
        vps[0].flags = LEVEL_FLAG_ACTIVE;
        for (int i=0; i<8; i++) disk.writeSector(newCluster * 8 + i, vpsBuf);
        lout << "Added level '" << name << "' (ID: " << newLevelID << ", extended chain)\n";
    }
    
    void branchLevel(string folderName, string parentLevelName, string newLevelName) {
        if (!disk.isOpen()) return;
        
        uint64_t folderCluster = findFolderCluster(folderName);
        if (!folderCluster) {
            lout << "Folder '" << folderName << "' not found.\n";
            return;
        }
        
        VersionEntry* parentEntry = nullptr;
        VersionEntry foundEntry;
        uint64_t parentLevelID = 0;
        
        vector<uint64_t> chain = getChain(folderCluster);
        for (uint64_t c : chain) {
        char vpsBuf[SECTOR_SIZE];
        for (int i = 0; i < 8; i++) {
            disk.readSector(c * 8 + i, vpsBuf);
            VersionEntry* vps = (VersionEntry*)vpsBuf;
            for (int j = 0; j < SECTOR_SIZE / sizeof(VersionEntry); j++) {
                if (vps[j].isActive && string(vps[j].versionName) == parentLevelName) {
                    parentEntry = &vps[j];
                    foundEntry = vps[j];
                    parentLevelID = vps[j].levelID;
                    goto found_parent;
                }
            }
        }
    }
found_parent:
        if (!parentEntry) {
            lout << "Parent level '" << parentLevelName << "' not found.\n";
            return;
        }
        
        uint64_t newContentCluster = allocCluster();
        if (newContentCluster == 0) {
            lout << "Disk full. Cannot branch level.\n";
            return;
        }
        
        DirEntry emptyContent[CLUSTER_SIZE / sizeof(DirEntry)];
        memset(emptyContent, 0, sizeof(emptyContent));
        for (int s = 0; s < SECTORS_PER_CLUSTER; s++) {
            disk.writeSector(newContentCluster * SECTORS_PER_CLUSTER + s,
                ((char*)emptyContent) + s * SECTOR_SIZE);
        }
        
        uint64_t newLevelID = sb.nextLevelID++;
        sb.totalLevels++;
        
        SYSTEMTIME st;
        GetSystemTime(&st);
        FILETIME ft;
        SystemTimeToFileTime(&st, &ft);
        uint64_t timestamp = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        
        vector<uint64_t> regChain = getChain(sb.levelRegistryCluster);
        for (uint64_t c : regChain) {
            char registryBuf[CLUSTER_SIZE];
            LevelDescriptor* registry = (LevelDescriptor*)registryBuf;
            for (int s = 0; s < SECTORS_PER_CLUSTER; s++) {
                disk.readSector(c * SECTORS_PER_CLUSTER + s,
                    registryBuf + s * SECTOR_SIZE);
            }
            for (int j = 0; j < CLUSTER_SIZE / sizeof(LevelDescriptor); j++) {
                if (registry[j].levelID == 0 && !(registry[j].flags & LEVEL_FLAG_ACTIVE)) {
                    strcpy(registry[j].name, newLevelName.c_str());
                    registry[j].levelID = newLevelID;
                    registry[j].parentLevelID = parentLevelID;
                    registry[j].rootContentCluster = newContentCluster;
                    registry[j].createTime = timestamp;
                    registry[j].modTime = timestamp;
                    registry[j].flags = LEVEL_FLAG_ACTIVE | LEVEL_FLAG_DERIVED;
                    registry[j].refCount = 1;
                    registry[j].childCount = 0;
                    
                    for (int s = 0; s < SECTORS_PER_CLUSTER; s++) {
                        disk.writeSector(c * SECTORS_PER_CLUSTER + s,
                            registryBuf + s * SECTOR_SIZE);
                    }
                    goto registry_done;
                }
            }
        }
        registry_done:
        
        for (uint64_t c : chain) {
        char vpsBuf[SECTOR_SIZE];
        for (int i = 0; i < 8; i++) {
            disk.readSector(c * 8 + i, vpsBuf);
            VersionEntry* vps = (VersionEntry*)vpsBuf;
            for (int j = 0; j < SECTOR_SIZE / sizeof(VersionEntry); j++) {
                if (!vps[j].isActive) {
                    // Found free slot
                    strcpy(vps[j].versionName, newLevelName.c_str());
                    vps[j].contentTableCluster = newContentCluster; // Content is empty as per creation
                    vps[j].isActive = 1;
                    vps[j].createTime = time(0);
                    vps[j].modTime = time(0);
                    vps[j].levelID = newLevelID;
                    vps[j].parentLevelID = parentLevelID;
                    vps[j].flags = LEVEL_FLAG_ACTIVE | LEVEL_FLAG_DERIVED;
                    disk.writeSector(c * 8 + i, vpsBuf);
                    
                    writeSuperBlock();
                        lout << "Branched level '" << newLevelName << "' (ID: " << newLevelID 
                             << ") from '" << parentLevelName << "' (ID: " << parentLevelID << ")\n";
                        return;
                    }
                }
            }
        }
        
        lout << "No space in version table. Cannot branch.\n";
    }
    
    uint64_t findFolderCluster(string folderName) {
        if (folderName == ".") return context.currentDirCluster;
        DirEntry entries[SECTOR_SIZE/sizeof(DirEntry)];
        for (int i=0; i<8; i++) {
            memset(entries, 0, sizeof(entries));
            disk.readSector(context.currentContentCluster * 8 + i, entries);
            for (int j=0; j<SECTOR_SIZE/sizeof(DirEntry); j++) {
                if (entries[j].type == TYPE_LEVELED_DIR && string(entries[j].name) == folderName) {
                    return entries[j].startCluster;
                }
            }
        }
        return 0;
    }
    
    void levelAdd(string folderName, string levelName) {
        if (!disk.isOpen()) return;
        uint64_t cluster = findFolderCluster(folderName);
        if (!cluster) {
            lout << "Folder '" << folderName << "' not found.\n";
            return;
        }
        addLevel(cluster, levelName);
    }
    
    void levelBranch(string folderName, string parentLevel, string newLevel) {
        branchLevel(folderName, parentLevel, newLevel);
    }
    
    void linkLevel(string dir1Path, string dir2Path, string sharedLevelName) {
        if (!disk.isOpen()) return;
        
        if (!(context.currentFolderPerms & PERM_WRITE)) {
            lout << "Permission denied: current folder is read-only.\n";
            return;
        }
        
        PathResult res1 = resolvePath(dir1Path);
        PathResult res2 = resolvePath(dir2Path);
        
        if (!res1.valid || !res2.valid) {
            lout << "Invalid directory path.\n";
            return;
        }
        
        // Find both directory clusters
        vector<DirEntry> entries1 = readDirEntries(res1.parentCluster);
        vector<DirEntry> entries2 = readDirEntries(res2.parentCluster);
        
        uint64_t dir1Cluster = 0;
        uint64_t dir2Cluster = 0;
        
        // Find first directory
        for (const auto& e : entries1) {
            if (e.type == TYPE_LEVELED_DIR && string(e.name) == res1.name) {
                dir1Cluster = e.startCluster;
                break;
            }
        }
        
        // Find second directory
        for (const auto& e : entries2) {
            if (e.type == TYPE_LEVELED_DIR && string(e.name) == res2.name) {
                dir2Cluster = e.startCluster;
                break;
            }
        }
        
        if (!dir1Cluster || !dir2Cluster) {
            lout << "One or both directories not found.\n";
            return;
        }
        
        if (dir1Cluster == dir2Cluster) {
            lout << "Cannot link a directory to itself.\n";
            return;
        }
        
        // Allocate shared content table cluster
        uint64_t sharedContentCluster = allocCluster();
        if (sharedContentCluster == 0) {
            lout << "Disk full. Cannot create shared level.\n";
            return;
        }
        
        // Initialize shared content cluster
        DirEntry emptyEntries[SECTOR_SIZE/sizeof(DirEntry)];
        memset(emptyEntries, 0, sizeof(emptyEntries));
        for (int i=0; i<8; i++) {
            disk.writeSector(sharedContentCluster * 8 + i, emptyEntries);
        }
        
        // Add level to first directory
        bool added1 = addLevelWithCluster(dir1Cluster, sharedLevelName, sharedContentCluster);
        if (!added1) {
            lout << "Failed to add level to first directory.\n";
            return;
        }
        
        // Add level to second directory (pointing to SAME cluster)
        bool added2 = addLevelWithCluster(dir2Cluster, sharedLevelName, sharedContentCluster);
        if (!added2) {
            lout << "Failed to add level to second directory.\n";
            return;
        }
        
        lout << "Created shared level '" << sharedLevelName << "' linking:\n";
        lout << "  " << dir1Path << " <-> " << dir2Path << "\n";
        lout << "Changes in one will appear in the other (DAG structure).\n";
    }
    
    bool addLevelWithCluster(uint64_t dirCluster, string levelName, uint64_t contentCluster) {
        // Follow LAT chain to find free slot or extend
        vector<uint64_t> chain = getChain(dirCluster);
        
        for (uint64_t c : chain) {
            char vpsBuf[SECTOR_SIZE];
            VersionEntry* vps = (VersionEntry*)vpsBuf;
            for (int i=0; i<8; i++) {
                 disk.readSector(c * 8 + i, vpsBuf);
                 for(int j=0; j<SECTOR_SIZE/sizeof(VersionEntry); j++) {
                     if (!vps[j].isActive) {
                         // Found free slot
                         strcpy(vps[j].versionName, levelName.c_str());
                         vps[j].contentTableCluster = contentCluster;
                         vps[j].isActive = 1;
                         disk.writeSector(c * 8 + i, vpsBuf);
                         return true;
                     }
                 }
            }
        }
        
        // No free slots - extend chain
        uint64_t lastCluster = chain.back();
        uint64_t newCluster = allocCluster();
        if (newCluster == 0) return false;
        
        setLATEntry(lastCluster, newCluster);
        
        // Initialize new cluster
    char vpsBuf[SECTOR_SIZE];
    VersionEntry* vps = (VersionEntry*)vpsBuf;
    memset(vpsBuf, 0, sizeof(vpsBuf));
    strcpy(vps[0].versionName, levelName.c_str());
    vps[0].contentTableCluster = contentCluster;
    vps[0].isActive = 1;
    for (int i=0; i<8; i++) disk.writeSector(newCluster * 8 + i, vpsBuf);
        
        return true;
    }

    vector<DirEntry> readDirEntries(uint64_t cluster) {
        vector<DirEntry> entries;
        vector<uint64_t> chain = getChain(cluster);
        
        for (uint64_t c : chain) {
            for (int i = 0; i < 8; i++) { 
                uint8_t buffer[SECTOR_SIZE];
                disk.readSector(c * 8 + i, buffer);
                DirEntry* de = (DirEntry*)buffer;
                for (int j = 0; j < SECTOR_SIZE / sizeof(DirEntry); j++) {
                    if (de[j].type != TYPE_FREE) {
                        entries.push_back(de[j]);
                    }
                }
            }
        }
        return entries;
    }

    void read(string path) {
        if (!disk.isOpen()) return;
        PathResult res = resolvePath(path);
        if (!res.valid) { lout << "Invalid path.\n"; return; }
        
        vector<DirEntry> entries = readDirEntries(res.parentCluster);
        bool found = false;
        DirEntry fileEntry;
        uint64_t fileCluster = 0;
        FileParts fp = parseFileName(res.name);
        
        for (const auto& e : entries) {
            if (matchesFile(e, fp)) {
                fileEntry = e;
                fileCluster = e.startCluster;
                found = true;
                break;
            }
        }
        
        if (!found) { lout << "File not found.\n"; return; }
        
        // Check read permission
        if (!(fileEntry.attributes & PERM_READ)) {
            lout << "Permission denied: no read access.\n";
            return;
        }
        
        ScopedFileLock readLock(lockManager, fileCluster, path, LFS_LOCK_SHARED, 5000);
        if (!readLock.isHeld()) {
            lout << "File is locked by another process.\n";
            return;
        }
        
        // Follow symlink (with loop detection)
        int symlinkDepth = 0;
        while (fileEntry.type == TYPE_SYMLINK && symlinkDepth < 10) {
            if (fileEntry.startCluster == 0) {
                lout << "Broken symlink.\n";
                return;
            }
            
            // Read target path from symlink cluster
            char targetPath[CLUSTER_SIZE];
            memset(targetPath, 0, CLUSTER_SIZE);
            for (int i=0; i<8; i++) {
                disk.readSector(fileEntry.startCluster * 8 + i, targetPath + (i * SECTOR_SIZE));
            }
            
            // Resolve target
            PathResult targetRes = resolvePath(string(targetPath));
            if (!targetRes.valid) {
                lout << "Broken symlink: target '" << targetPath << "' not found.\n";
                return;
            }
            
            // Get target entry
            vector<DirEntry> targetEntries = readDirEntries(targetRes.parentCluster);
            found = false;
            for (const auto& e : targetEntries) {
                if ((e.type == TYPE_FILE || e.type == TYPE_HARDLINK) && string(e.name) == targetRes.name) {
                    fileEntry = e;
                    found = true;
                    break;
                }
            }
            
            if (!found) {
                lout << "Broken symlink: target not found.\n";
                return;
            }
            symlinkDepth++;
        }
        
        if (symlinkDepth >= 10) {
            lout << "Symlink loop detected or max depth exceeded.\n";
            return;
        }
        
        // Now read the actual file (regular or hardlink)
        if (fileEntry.size == 0) return;
        
        vector<uint64_t> chain = getChain(fileEntry.startCluster);
        uint64_t remaining = fileEntry.size;
        
        for (uint64_t c : chain) {
            if (remaining == 0) break;
            uint64_t toRead = std::min((uint64_t)CLUSTER_SIZE, remaining);
            
            char buffer[CLUSTER_SIZE];
            for (int i=0; i<8; i++) {
                disk.readSector(c*8 + i, buffer + (i*SECTOR_SIZE));
            }
            lout.write(buffer, toRead);
            remaining -= toRead;
        }
        lout << endl;
        
        updateAccessTime(res.parentCluster, res.name);
    }

    void deleteLevel(string folderName, string levelName) {
        uint64_t folderCluster = findFolderCluster(folderName);
        if (folderCluster == 0) {
            lout << "Folder '" << folderName << "' not found.\n";
            return;
        }
        
        char vpsBuf[SECTOR_SIZE];
        VersionEntry* vps = (VersionEntry*)vpsBuf;
        
        for (int s = 0; s < 8; s++) {
            disk.readSector(folderCluster * 8 + s, vpsBuf);
            for (int j = 0; j < SECTOR_SIZE / sizeof(VersionEntry); j++) {
                if (vps[j].isActive && string(vps[j].versionName) == levelName) {
                    DirEntry contentEntries[SECTOR_SIZE/sizeof(DirEntry)];
                    bool hasContents = false;
                    
                    if (vps[j].contentTableCluster != 0) {
                        for(int cs=0; cs<8; cs++) {
                            disk.readSector(vps[j].contentTableCluster*8 + cs, contentEntries);
                            for(int e=0; e<SECTOR_SIZE/sizeof(DirEntry); e++) {
                                if(contentEntries[e].type != TYPE_FREE) {
                                    hasContents = true;
                                    goto level_check_done;
                                }
                            }
                        }
                    }
level_check_done:
                    if (hasContents) {
                        lout << "Level '" << levelName << "' is not empty. Delete contents first.\n";
                        return;
                    }
                    
                    if (vps[j].contentTableCluster != 0) {
                        freeChain(vps[j].contentTableCluster);
                    }
                    
                    memset(&vps[j], 0, sizeof(VersionEntry));
                    disk.writeSector(folderCluster * 8 + s, vpsBuf);
                    lout << "Deleted level '" << levelName << "' from '" << folderName << "'.\n";
                    return;
                }
            }
        }
        lout << "Level '" << levelName << "' not found in '" << folderName << "'.\n";
    }

    bool isLevelShared(uint64_t contentCluster, uint64_t excludeFolderCluster) {
        vector<uint64_t> chain = getChain(context.rootContentCluster);
        for (uint64_t c : chain) {
            for (int i = 0; i < 8; i++) {
                DirEntry entries[SECTOR_SIZE / sizeof(DirEntry)];
                disk.readSector(c * 8 + i, entries);
                for (int j = 0; j < SECTOR_SIZE / sizeof(DirEntry); j++) {
                    if (entries[j].type == TYPE_LEVELED_DIR && 
                        entries[j].startCluster != 0 && 
                        entries[j].startCluster != excludeFolderCluster) {
                        
                        char vpsBuf[SECTOR_SIZE];
                        VersionEntry* vps = (VersionEntry*)vpsBuf;
                        for (int s = 0; s < 8; s++) {
                            disk.readSector(entries[j].startCluster * 8 + s, vpsBuf);
                            for (int v = 0; v < SECTOR_SIZE / sizeof(VersionEntry); v++) {
                                if (vps[v].isActive && vps[v].contentTableCluster == contentCluster) {
                                    return true;
                                }
                            }
                        }
                    }
                }
            }
        }
        return false;
    }

    void del(string path, bool recursive) {
        if (!disk.isOpen()) return;
        
        if (!(context.currentFolderPerms & PERM_WRITE)) {
            lout << "Permission denied: current folder is read-only.\n";
            return;
        }
        
        size_t colonPos = path.find(':');
        if (colonPos != string::npos && colonPos > 0) {
            string folderName = path.substr(0, colonPos);
            string levelName = path.substr(colonPos + 1);
            deleteLevel(folderName, levelName);
            return;
        }
        
        PathResult res = resolvePath(path);
        if (!res.valid) { lout << "Item not found.\n"; return; }

        DirEntry entries[SECTOR_SIZE/sizeof(DirEntry)];
        uint64_t foundCluster = 0;
        int foundSector = -1, foundIdx = -1;
        FileParts fp = parseFileName(res.name);
        
        // Follow LAT chain for deletion search
        vector<uint64_t> chain = getChain(res.parentCluster);
        for (uint64_t c : chain) {
            for (int i=0; i<8; i++) {
                disk.readSector(c * 8 + i, entries);
                for (int j=0; j<SECTOR_SIZE/sizeof(DirEntry); j++) {
                    if (matchesEntry(entries[j], fp)) {
                        if (entries[j].type == TYPE_LEVELED_DIR && !recursive) {
                             char vpsBuf[SECTOR_SIZE];
                             VersionEntry* vps = (VersionEntry*)vpsBuf;
                             bool hasContents = false;
                             for(int k=0; k<8 && !hasContents; k++) {
                                 disk.readSector(entries[j].startCluster*8 + k, vpsBuf);
                                 for(int l=0; l<SECTOR_SIZE/sizeof(VersionEntry) && !hasContents; l++) {
                                     if(vps[l].isActive && vps[l].contentTableCluster != 0) {
                                         DirEntry contentEntries[SECTOR_SIZE/sizeof(DirEntry)];
                                         for(int cs=0; cs<8 && !hasContents; cs++) {
                                             disk.readSector(vps[l].contentTableCluster*8 + cs, contentEntries);
                                             for(int e=0; e<SECTOR_SIZE/sizeof(DirEntry); e++) {
                                                 if(contentEntries[e].type != TYPE_FREE) {
                                                     hasContents = true;
                                                     break;
                                                 }
                                             }
                                         }
                                     }
                                 }
                             }
                             if (hasContents) {
                                 lout << "Folder '" << res.name << "' is not empty. Use 'del -r " << path << "'.\n";
                                 return;
                             }
                        }
                        
                        // Check write permission for files
                        if (entries[j].type == TYPE_FILE && !(entries[j].attributes & PERM_WRITE)) {
                            lout << "Permission denied: no write access to delete '" << res.name << "'.\n";
                            return;
                        }
                        
                        // Acquire exclusive lock for deletion
                        ScopedFileLock deleteLock(lockManager, entries[j].startCluster, path, LFS_LOCK_EXCLUSIVE, 5000);
                        if (entries[j].type == TYPE_FILE && entries[j].startCluster != 0) {
                            if (!deleteLock.isHeld()) {
                                lout << "Cannot delete: file is locked by another operation.\n";
                                return;
                            }
                        }
                        
                        // Handle hardlink reference counting
                        if (entries[j].type == TYPE_HARDLINK || entries[j].type == TYPE_FILE) {
                            if (entries[j].attributes > 1) {
                                // Decrement reference count for all links pointing to this data
                                uint64_t dataCluster = entries[j].startCluster;
                                decrementRefCount(dataCluster);
                                lout << "Deleted hardlink " << res.name << " (refcount decremented).\n";
                            } else {
                                lout << "Deleted " << res.name << ".\n";
                            }
                        } else if (entries[j].type == TYPE_SYMLINK) {
                            lout << "Deleted symlink " << res.name << ".\n";
                        } else {
                            lout << "Deleted " << res.name << ".\n";
                        }
                        
                        if (entries[j].startCluster != 0 && entries[j].type != TYPE_SYMLINK) {
                            if (entries[j].type == TYPE_LEVELED_DIR) {
                                char vpsBuf[SECTOR_SIZE];
                                VersionEntry* vps = (VersionEntry*)vpsBuf;
                                for (int s = 0; s < 8; s++) {
                                    disk.readSector(entries[j].startCluster * 8 + s, vpsBuf);
                                    for (int v = 0; v < SECTOR_SIZE/sizeof(VersionEntry); v++) {
                                        if (vps[v].isActive && vps[v].contentTableCluster != 0) {
                                            if (isLevelShared(vps[v].contentTableCluster, entries[j].startCluster)) {
                                                lout << "  Level '" << vps[v].versionName << "' is shared, detaching (not deleted).\n";
                                            } else {
                                                freeChain(vps[v].contentTableCluster);
                                            }
                                        }
                                    }
                                }
                                freeChain(entries[j].startCluster);
                            } else {
                                freeChain(entries[j].startCluster);
                            }
                        }
                        
                        updateLevelStats(context.currentLevelID, -((int64_t)entries[j].size), -1);
                        
                        entries[j].type = TYPE_FREE;
                        memset(&entries[j], 0, sizeof(DirEntry));
                        disk.writeSector(c * 8 + i, entries);
                        return;
                    }
                }
            }
        }
        lout << "Target not found.\n";
    }
    
    void decrementRefCount(uint64_t dataCluster) {
        // Find all entries using this data cluster and decrement their refcount
        vector<uint64_t> dirChain = getChain(context.rootContentCluster);
        for (uint64_t c : dirChain) {
            for (int i=0; i<8; i++) {
                DirEntry entries[SECTOR_SIZE/sizeof(DirEntry)];
                disk.readSector(c * 8 + i, entries);
                bool modified = false;
                for (int j=0; j<SECTOR_SIZE/sizeof(DirEntry); j++) {
                    if ((entries[j].type == TYPE_FILE || entries[j].type == TYPE_HARDLINK) &&
                        entries[j].startCluster == dataCluster && entries[j].attributes > 0) {
                        entries[j].attributes--;
                        modified = true;
                    }
                }
                if (modified) disk.writeSector(c * 8 + i, entries);
            }
        }
    }

    void move(string srcPath, string dstPath) {
        if (!disk.isOpen()) return;
        
        if (srcPath.find(':') != string::npos || dstPath.find(':') != string::npos) {
            lout << "Cannot move levels. Use level commands instead.\n";
            return;
        }
        
        PathResult srcRes = resolvePath(srcPath);
        if (!srcRes.valid) { lout << "Source not found.\n"; return; }
        
        PathResult dstRes = resolvePath(dstPath);
        if (!dstRes.valid) { lout << "Invalid destination path.\n"; return; }

        DirEntry srcEntry;
        bool found = false;
        DirEntry entries[SECTOR_SIZE/sizeof(DirEntry)];
        
        // Find Source - Follow LAT chain
        vector<uint64_t> srcChain = getChain(srcRes.parentCluster);
        for (uint64_t c : srcChain) {
            for (int i=0; i<8; i++) {
                disk.readSector(c * 8 + i, entries);
                for (int j=0; j<SECTOR_SIZE/sizeof(DirEntry); j++) {
                    if (entries[j].type != TYPE_FREE && string(entries[j].name) == srcRes.name) {
                        // Acquire exclusive lock for move operation
                        ScopedFileLock moveLock(lockManager, entries[j].startCluster, srcPath, LFS_LOCK_EXCLUSIVE, 5000);
                        if (entries[j].type == TYPE_FILE && entries[j].startCluster != 0) {
                            if (!moveLock.isHeld()) {
                                lout << "Cannot move: file is in use.\n";
                                return;
                            }
                        }
                        srcEntry = entries[j];
                        found = true;
                        entries[j].type = TYPE_FREE;
                        disk.writeSector(c * 8 + i, entries);
                        goto found_src;
                    }
                }
            }
        }
found_src:
        if (!found) { lout << "Source not found.\n"; return; }
        
        // Write to Dest - Follow LAT chain
        vector<uint64_t> dstChain = getChain(dstRes.parentCluster);
        for (uint64_t c : dstChain) {
            for (int i=0; i<8; i++) {
                disk.readSector(c * 8 + i, entries);
                for (int j=0; j<SECTOR_SIZE/sizeof(DirEntry); j++) {
                    if (entries[j].type == TYPE_FREE) {
                        entries[j] = srcEntry;
                        strcpy(entries[j].name, dstRes.name.c_str());
                        disk.writeSector(c * 8 + i, entries);
                        lout << "Moved " << srcPath << " to " << dstPath << endl;
                        return;
                    }
                }
            }
        }
    }

    void levelRemove(string folderName, string levelName) {
        if (!disk.isOpen()) return;
        uint64_t cluster = findFolderCluster(folderName);
        if (!cluster) {
            lout << "Folder '" << folderName << "' not found.\n";
            return;
        }
        char vpsBuf[SECTOR_SIZE];
        VersionEntry* vps = (VersionEntry*)vpsBuf;
        for (int i=0; i<8; i++) {
             disk.readSector(cluster * 8 + i, vpsBuf);
             vps = (VersionEntry*)vpsBuf;
             for(int j=0; j<SECTOR_SIZE/sizeof(VersionEntry); j++) {
                 if (vps[j].isActive && string(vps[j].versionName) == levelName) {
                     if (levelName == "master") {
                         lout << "Cannot remove master level.\n";
                         return;
                     }
                     vps[j].isActive = 0;
                     disk.writeSector(cluster * 8 + i, vpsBuf);
                     lout << "Removed level " << levelName << " from " << folderName << endl;
                     return;
                 }
             }
        }
        lout << "Level '" << levelName << "' not found.\n";
    }

    void levelRename(string folderName, string oldName, string newName) {
        if (!disk.isOpen()) return;
        uint64_t cluster = findFolderCluster(folderName);
        if (!cluster) {
            lout << "Folder '" << folderName << "' not found.\n";
            return;
        }
        char vpsBuf[SECTOR_SIZE];
        VersionEntry* vps = (VersionEntry*)vpsBuf;
        for (int i=0; i<8; i++) {
             disk.readSector(cluster * 8 + i, vpsBuf);
             vps = (VersionEntry*)vpsBuf;
             for(int j=0; j<SECTOR_SIZE/sizeof(VersionEntry); j++) {
                 if (vps[j].isActive && string(vps[j].versionName) == oldName) {
                     strcpy(vps[j].versionName, newName.c_str());
                     disk.writeSector(cluster * 8 + i, vpsBuf);
                     lout << "Renamed level " << oldName << " to " << newName << " in " << folderName << "\n";
                     return;
                 }
             }
        }
        lout << "Level '" << oldName << "' not found.\n";
    }

    void rename(string path, string newName) {
        if (!disk.isOpen()) return;
        
        if (!(context.currentFolderPerms & PERM_WRITE)) {
            lout << "Permission denied: current folder is read-only.\n";
            return;
        }
        
        size_t colonPos = path.find(':');
        if (colonPos != string::npos && colonPos > 0) {
            string folderName = path.substr(0, colonPos);
            string levelName = path.substr(colonPos + 1);
            renameLevel(folderName, levelName, newName);
            return;
        }
        
        PathResult res = resolvePath(path);
        if (!res.valid) { lout << "Item not found.\n"; return; }
        
        if (newName.length() > 23) {
            lout << "Name too long (max 23 characters).\n";
            return;
        }
        
        vector<uint64_t> chain = getChain(res.parentCluster);
        for (uint64_t c : chain) {
            for (int i = 0; i < 8; i++) {
                DirEntry entries[SECTOR_SIZE / sizeof(DirEntry)];
                disk.readSector(c * 8 + i, entries);
                for (int j = 0; j < SECTOR_SIZE / sizeof(DirEntry); j++) {
                    if (entries[j].type != TYPE_FREE && string(entries[j].name) == res.name) {
                        if (!(entries[j].attributes & PERM_WRITE) && entries[j].type == TYPE_FILE) {
                            lout << "Permission denied: no write access.\n";
                            return;
                        }
                        
                        // Acquire exclusive lock for rename operation
                        ScopedFileLock renameLock(lockManager, entries[j].startCluster, path, LFS_LOCK_EXCLUSIVE, 5000);
                        if (entries[j].type == TYPE_FILE && entries[j].startCluster != 0) {
                            if (!renameLock.isHeld()) {
                                lout << "Cannot rename: file is in use.\n";
                                return;
                            }
                        }
                        
                        strncpy(entries[j].name, newName.c_str(), 23);
                        entries[j].name[23] = '\0';
                        entries[j].modTime = time(0);
                        disk.writeSector(c * 8 + i, entries);
                        
                        string typeStr = "item";
                        if (entries[j].type == TYPE_FILE) typeStr = "file";
                        else if (entries[j].type == TYPE_LEVELED_DIR) typeStr = "folder";
                        else if (entries[j].type == TYPE_SYMLINK) typeStr = "symlink";
                        
                        lout << "Renamed " << typeStr << " '" << res.name << "' to '" << newName << "'.\n";
                        return;
                    }
                }
            }
        }
        lout << "Item not found.\n";
    }
    
    void renameLevel(string folderName, string levelName, string newName) {
        uint64_t folderCluster = findFolderCluster(folderName);
        if (folderCluster == 0) {
            lout << "Folder '" << folderName << "' not found.\n";
            return;
        }
        
        if (newName.length() > 31) {
            lout << "Level name too long (max 31 characters).\n";
            return;
        }
        
        char vpsBuf[SECTOR_SIZE];
        VersionEntry* vps = (VersionEntry*)vpsBuf;
        
        for (int s = 0; s < 8; s++) {
            disk.readSector(folderCluster * 8 + s, vpsBuf);
            for (int j = 0; j < SECTOR_SIZE / sizeof(VersionEntry); j++) {
                if (vps[j].isActive && string(vps[j].versionName) == levelName) {
                    strncpy(vps[j].versionName, newName.c_str(), 31);
                    vps[j].versionName[31] = '\0';
                    vps[j].modTime = time(0);
                    disk.writeSector(folderCluster * 8 + s, vpsBuf);
                    lout << "Renamed level '" << levelName << "' to '" << newName << "'.\n";
                    return;
                }
            }
        }
        lout << "Level '" << levelName << "' not found in '" << folderName << "'.\n";
    }


    void current() {
        if (!disk.isOpen()) {
            lout << "Not mounted.\n";
            return;
        }
        lout << "Path: " << context.currentPath << "\n";
        lout << "Level: " << context.currentVersion << "\n";
        lout << "Directory Cluster: " << context.currentDirCluster << "\n";
        lout << "Content Cluster: " << context.currentContentCluster << "\n";
    }

    void write(string path) {
        if (!disk.isOpen()) return;
        
        // Check if current folder allows write (for new files)
        if (!(context.currentFolderPerms & PERM_WRITE)) {
            lout << "Permission denied: current folder is read-only.\n";
            return;
        }
        
        PathResult res = resolvePath(path);
        if (!res.valid) { lout << "Invalid path location.\n"; return; }
        string name = res.name;
        uint64_t contentCluster = res.parentCluster;
        
        DirEntry entries[SECTOR_SIZE/sizeof(DirEntry)];
        int foundSector = -1, foundIdx = -1;
        bool isNew = true;
        
        // Scan directory (following chain) for name or free slot
        // NOTE: readDirEntries simplifies this but we need sector location to Update.
        // So we must manually scan.
        vector<uint64_t> dirChain = getChain(contentCluster);
        DirEntry foundEntry;

        // First pass: look for existing file
        for (uint64_t c : dirChain) {
            for (int i=0; i<8; i++) {
                uint8_t sec[SECTOR_SIZE];
                disk.readSector(c*8 + i, sec);
                DirEntry* des = (DirEntry*)sec;
                for (int j=0; j<SECTOR_SIZE/sizeof(DirEntry); j++) {
                    if (des[j].type == TYPE_FILE && string(des[j].name) == name) {
                        foundEntry = des[j];
                        isNew = false;
                        foundSector = (c*8) + i; // absolute sector
                        foundIdx = j;
                        goto scan_done;
                    }
                }
            }
        }
        
        // Second pass: look for free slot
        if (isNew) {
            for (uint64_t c : dirChain) {
                 for (int i=0; i<8; i++) {
                     uint8_t sec[SECTOR_SIZE];
                     disk.readSector(c*8 + i, sec);
                     DirEntry* des = (DirEntry*)sec;
                     for (int j=0; j<SECTOR_SIZE/sizeof(DirEntry); j++) {
                         if (des[j].type == TYPE_FREE) {
                             foundSector = (c*8) + i;
                             foundIdx = j;
                             goto scan_done;
                         }
                     }
                 }
            }
            lout << "Directory full.\n";
            return;
        }

scan_done:
        // Check write permission for existing files
        if (!isNew && !(foundEntry.attributes & PERM_WRITE)) {
            lout << "Permission denied: no write access to '" << name << "'.\n";
            return;
        }
        
        uint64_t lockCluster = isNew ? 0 : foundEntry.startCluster;
        ScopedFileLock writeLock(lockManager, lockCluster, path, LFS_LOCK_EXCLUSIVE, 5000);
        if (lockCluster != 0 && !writeLock.isHeld()) {
            lout << "File is locked by another process.\n";
            return;
        }
        
        lout << "--- Editor: " << name << " ---\n";
        lout << "Type content. End with line '.done'\n";
        string content, line;
        while (getline(cin, line)) {
            if (line == ".done") break;
            content += line + "\n";
        }
        if (content.empty()) { lout << "No content.\n"; return; }
        
        vector<uint8_t> data(content.begin(), content.end());

        // Log operation to journal
        uint64_t txId = journal->logOperation(OP_WRITE, contentCluster, name);

        // Update Entry
        uint8_t sectorData[SECTOR_SIZE];
        disk.readSector(foundSector, sectorData);
        DirEntry* entryPtr = (DirEntry*)sectorData + foundIdx;
        
        if (isNew) {
            entryPtr->type = TYPE_FILE;
            strncpy(entryPtr->name, name.c_str(), 23);
            entryPtr->startCluster = allocCluster();
            entryPtr->createTime = time(0);
            entryPtr->attributes = PERM_DEFAULT;
            if (entryPtr->startCluster == 0) { lout << "Disk full.\n"; return; }
        }
        
        entryPtr->modTime = time(0);
        entryPtr->size = data.size();
        
        uint64_t startCluster = entryPtr->startCluster;
        disk.writeSector(foundSector, sectorData);
        
        uint64_t current = startCluster;
        uint64_t offset = 0;
        uint64_t total = data.size();
        
        while (offset < total) {
             uint64_t chunk = std::min((uint64_t)CLUSTER_SIZE, total - offset);
             uint8_t buffer[CLUSTER_SIZE];
             memset(buffer, 0, CLUSTER_SIZE);
             memcpy(buffer, data.data() + offset, chunk);
             
             for (int i=0; i<8; i++) {
                 disk.writeSector(current*8 + i, buffer + (i*SECTOR_SIZE));
             }
             
             offset += chunk;
             
             if (offset < total) {
                 uint64_t next = getLATEntry(current);
                 if (next == LAT_END || next == 0) {
                     next = allocCluster();
                     if (next == 0) { lout << "Disk full during write.\n"; break; }
                     setLATEntry(current, next);
                 }
                 current = next;
             }
        }
        setLATEntry(current, LAT_END);
        
        journal->commitOperation(txId);
        
        lout << "Written " << total << " bytes.\n";
    }

    void writeInsert(string path) {
        if (!disk.isOpen()) return;
        
        PathResult res = resolvePath(path);
        if (!res.valid) { lout << "File not found.\n"; return; }
        
        DirEntry targetEntry;
        uint64_t entrySector = 0;
        int entryIdx = -1;
        bool found = false;
        
        vector<uint64_t> chain = getChain(res.parentCluster);
        for (uint64_t c : chain) {
            for (int i = 0; i < 8; i++) {
                DirEntry entries[SECTOR_SIZE / sizeof(DirEntry)];
                disk.readSector(c * 8 + i, entries);
                for (int j = 0; j < SECTOR_SIZE / sizeof(DirEntry); j++) {
                    if (entries[j].type == TYPE_FILE && string(entries[j].name) == res.name) {
                        targetEntry = entries[j];
                        entrySector = c * 8 + i;
                        entryIdx = j;
                        found = true;
                        goto found_file;
                    }
                }
            }
        }
found_file:
        if (!found) { lout << "File not found.\n"; return; }
        
        if (!(targetEntry.attributes & PERM_WRITE)) {
            lout << "Permission denied: no write access.\n";
            return;
        }
        
        vector<string> lines;
        if (targetEntry.startCluster != 0 && targetEntry.size > 0) {
            string content;
            vector<uint64_t> fileChain = getChain(targetEntry.startCluster);
            uint64_t remaining = targetEntry.size;
            
            for (uint64_t c : fileChain) {
                if (remaining == 0) break;
                char buffer[CLUSTER_SIZE];
                for (int s = 0; s < 8; s++) {
                    disk.readSector(c * 8 + s, buffer + s * SECTOR_SIZE);
                }
                uint64_t toRead = min((uint64_t)CLUSTER_SIZE, remaining);
                content.append(buffer, toRead);
                remaining -= toRead;
            }
            
            stringstream ss(content);
            string line;
            while (getline(ss, line)) {
                lines.push_back(line);
            }
        }
        
        if (lines.empty()) {
            lout << "File is empty. Use 'write' instead.\n";
            return;
        }
        
        int selectedLine = 0;
        bool done = false;
        
        lout << "\n=== Insert Mode ===\n";
        lout << "Use UP/DOWN arrows to select line, ENTER to insert before it, ESC to cancel.\n\n";
        
        HANDLE hConsole = GetStdHandle(STD_INPUT_HANDLE);
        DWORD oldMode;
        GetConsoleMode(hConsole, &oldMode);
        SetConsoleMode(hConsole, 0);
        
        auto displayLines = [&]() {
            lout << "\033[2J\033[H";
            lout << "=== Insert Mode: " << res.name << " ===\n";
            lout << "UP/DOWN to navigate, ENTER to insert before selected line, ESC to cancel\n";
            lout << string(50, '-') << "\n";
            
            for (size_t i = 0; i < lines.size(); i++) {
                if ((int)i == selectedLine) {
                    lout << ">> ";
                } else {
                    lout << "   ";
                }
                lout << setw(3) << (i + 1) << ": ";
                if (lines[i].length() > 60) {
                    lout << lines[i].substr(0, 57) << "...";
                } else {
                    lout << lines[i];
                }
                lout << "\n";
            }
            lout << string(50, '-') << "\n";
            lout << "Selected: Line " << (selectedLine + 1) << "\n";
        };
        
        displayLines();
        
        while (!done) {
            INPUT_RECORD ir;
            DWORD count;
            if (ReadConsoleInput(hConsole, &ir, 1, &count)) {
                if (ir.EventType == KEY_EVENT && ir.Event.KeyEvent.bKeyDown) {
                    switch (ir.Event.KeyEvent.wVirtualKeyCode) {
                        case VK_UP:
                            if (selectedLine > 0) selectedLine--;
                            displayLines();
                            break;
                        case VK_DOWN:
                            if (selectedLine < (int)lines.size() - 1) selectedLine++;
                            displayLines();
                            break;
                        case VK_RETURN: {
                            SetConsoleMode(hConsole, oldMode);
                            lout << "\nEnter line to insert (or empty to cancel): ";
                            string newLine;
                            getline(cin, newLine);
                            
                            if (!newLine.empty()) {
                                lines.insert(lines.begin() + selectedLine, newLine);
                                
                                string newContent;
                                for (const auto& l : lines) {
                                    newContent += l + "\n";
                                }
                                
                                vector<uint8_t> data(newContent.begin(), newContent.end());
                                
                                DirEntry entries[SECTOR_SIZE / sizeof(DirEntry)];
                                disk.readSector(entrySector, entries);
                                entries[entryIdx].size = data.size();
                                entries[entryIdx].modTime = time(0);
                                disk.writeSector(entrySector, entries);
                                
                                uint64_t current = targetEntry.startCluster;
                                uint64_t offset = 0;
                                uint64_t total = data.size();
                                
                                while (offset < total) {
                                    uint64_t chunk = min((uint64_t)CLUSTER_SIZE, total - offset);
                                    uint8_t buffer[CLUSTER_SIZE];
                                    memset(buffer, 0, CLUSTER_SIZE);
                                    memcpy(buffer, data.data() + offset, chunk);
                                    
                                    for (int s = 0; s < 8; s++) {
                                        disk.writeSector(current * 8 + s, buffer + s * SECTOR_SIZE);
                                    }
                                    
                                    offset += chunk;
                                    
                                    if (offset < total) {
                                        uint64_t next = getLATEntry(current);
                                        if (next == LAT_END || next == 0) {
                                            next = allocCluster();
                                            if (next == 0) { lout << "Disk full.\n"; break; }
                                            setLATEntry(current, next);
                                        }
                                        current = next;
                                    }
                                }
                                setLATEntry(current, LAT_END);
                                
                                lout << "Inserted line at position " << (selectedLine + 1) << ".\n";
                            } else {
                                lout << "Cancelled.\n";
                            }
                            done = true;
                            break;
                        }
                        case VK_ESCAPE:
                            done = true;
                            lout << "\nCancelled.\n";
                            break;
                    }
                }
            }
        }
        
        SetConsoleMode(hConsole, oldMode);
    }

    void setVerbose(bool v) {
        disk.setVerbose(v);
        lout << "Disk logging " << (v ? "ENABLED" : "DISABLED") << ".\n";
    }


    void listAllLevels() {
        if (!disk.isOpen()) { lout << "Not mounted.\n"; return; }
        if (sb.levelRegistryCluster == 0) { lout << "No Level Registry.\n"; return; }
        
        lout << "\n=== Global Level Registry ===\n";
        lout << "  Total Levels: " << sb.totalLevels << "\n";
        lout << "  Next Level ID: " << sb.nextLevelID << "\n\n";
        lout << setw(4) << "ID" << "  " << setw(16) << left << "Name" 
             << setw(8) << "Parent" << setw(10) << "RefCount" << "Flags\n";
        lout << string(50, '-') << "\n";
        
        vector<uint64_t> chain = getChain(sb.levelRegistryCluster);
        
        for (uint64_t c : chain) {
            char registryBuf[SECTOR_SIZE];
            LevelDescriptor* registry = (LevelDescriptor*)registryBuf;
            for (int s = 0; s < 8; s++) {
                disk.readSector(c * 8 + s, registryBuf);
                for (int j = 0; j < SECTOR_SIZE / sizeof(LevelDescriptor); j++) {
                    if (registry[j].flags & LEVEL_FLAG_ACTIVE) {
                        string flagStr = "";
                        if (registry[j].flags & LEVEL_FLAG_SHARED) flagStr += "SHR ";
                        if (registry[j].flags & LEVEL_FLAG_LOCKED) flagStr += "LCK ";
                        if (registry[j].flags & LEVEL_FLAG_SNAPSHOT) flagStr += "SNP ";
                        if (registry[j].flags & LEVEL_FLAG_DERIVED) flagStr += "DRV ";
                        if (flagStr.empty()) flagStr = "ACT";
                        
                        lout << setw(4) << right << registry[j].levelID << "  "
                             << setw(16) << left << registry[j].name
                             << setw(8) << right << registry[j].parentLevelID
                             << setw(10) << registry[j].refCount
                             << flagStr << "\n";
                    }
                }
            }
        }
        lout << "\n";
    }
    
    void importFile(const string& hostPath, string lfsName = "") {
        if (!disk.isOpen()) { lout << "Not mounted.\n"; return; }
        
        if (!(context.currentFolderPerms & PERM_WRITE)) {
            lout << "Permission denied: current folder is read-only.\n";
            return;
        }
        
        string cleanPath = hostPath;
        if (cleanPath.size() >= 2 && cleanPath.front() == '"' && cleanPath.back() == '"') {
            cleanPath = cleanPath.substr(1, cleanPath.size() - 2);
        }
        
        FILE* hostFile = fopen(cleanPath.c_str(), "rb");
        if (!hostFile) {
            lout << "Cannot open host file: " << hostPath << "\n";
            return;
        }
        
        fseek(hostFile, 0, SEEK_END);
        uint64_t fileSize = ftell(hostFile);
        fseek(hostFile, 0, SEEK_SET);
        
        if (lfsName.empty()) {
            size_t lastSlash = hostPath.find_last_of("\\/");
            lfsName = (lastSlash != string::npos) ? hostPath.substr(lastSlash + 1) : hostPath;
        }
        
        string name = lfsName;
        string ext = "";
        size_t dotPos = lfsName.find_last_of('.');
        if (dotPos != string::npos && dotPos > 0) {
            name = lfsName.substr(0, dotPos);
            ext = lfsName.substr(dotPos + 1);
        }
        
        if (name.length() > 23) name = name.substr(0, 23);
        if (ext.length() > 3) ext = ext.substr(0, 3);
        
        vector<uint64_t> dirChain = getChain(context.currentContentCluster);
        int foundSector = -1, foundIdx = -1;
        bool isNew = true;
        DirEntry foundEntry;
        
        for (uint64_t c : dirChain) {
            for (int i = 0; i < 8; i++) {
                uint8_t sec[SECTOR_SIZE];
                disk.readSector(c * 8 + i, sec);
                DirEntry* des = (DirEntry*)sec;
                for (int j = 0; j < SECTOR_SIZE / sizeof(DirEntry); j++) {
                    if (des[j].type == TYPE_FILE && string(des[j].name) == name) {
                        foundEntry = des[j];
                        isNew = false;
                        foundSector = (c * 8) + i;
                        foundIdx = j;
                        goto import_scan_done;
                    }
                }
            }
        }
        
        for (uint64_t c : dirChain) {
            for (int i = 0; i < 8; i++) {
                uint8_t sec[SECTOR_SIZE];
                disk.readSector(c * 8 + i, sec);
                DirEntry* des = (DirEntry*)sec;
                for (int j = 0; j < SECTOR_SIZE / sizeof(DirEntry); j++) {
                    if (des[j].type == TYPE_FREE) {
                        foundSector = (c * 8) + i;
                        foundIdx = j;
                        goto import_scan_done;
                    }
                }
            }
        }
        lout << "Directory full.\n";
        fclose(hostFile);
        return;
        
import_scan_done:
        uint64_t txId = journal->logOperation(OP_WRITE, context.currentContentCluster, name);
        
        uint8_t sectorData[SECTOR_SIZE];
        disk.readSector(foundSector, sectorData);
        DirEntry* entryPtr = (DirEntry*)sectorData + foundIdx;
        
        if (isNew) {
            memset(entryPtr, 0, sizeof(DirEntry));
            entryPtr->type = TYPE_FILE;
            strncpy(entryPtr->name, name.c_str(), 23);
            strncpy(entryPtr->extension, ext.c_str(), 3);
            entryPtr->startCluster = allocCluster();
            entryPtr->createTime = time(0);
            entryPtr->attributes = PERM_DEFAULT;
            if (entryPtr->startCluster == 0) {
                lout << "Disk full.\n";
                fclose(hostFile);
                return;
            }
        }
        
        entryPtr->modTime = time(0);
        entryPtr->size = fileSize;
        
        uint64_t clustersNeeded = (fileSize + CLUSTER_SIZE - 1) / CLUSTER_SIZE;
        if (clustersNeeded == 0) clustersNeeded = 1;
        
        vector<uint64_t> clusterChain;
        clusterChain.reserve(clustersNeeded);
        
        uint64_t firstCluster = entryPtr->startCluster;
        if (firstCluster == 0) {
            firstCluster = allocCluster();
            if (firstCluster == 0) {
                lout << "Disk full.\n";
                fclose(hostFile);
                return;
            }
            entryPtr->startCluster = firstCluster;
        }
        clusterChain.push_back(firstCluster);
        
        for (uint64_t i = 1; i < clustersNeeded; i++) {
            uint64_t next = allocCluster();
            if (next == 0) {
                lout << "Disk full. Pre-allocation failed at cluster " << i << "\n";
                fclose(hostFile);
                return;
            }
            setLATEntry(clusterChain.back(), next);
            clusterChain.push_back(next);
        }
        setLATEntry(clusterChain.back(), LAT_END);
        
        disk.writeSector(foundSector, sectorData);
        
        TransferProgress progress(fileSize, "Import");
        
        vector<vector<uint8_t>> buffers(clustersNeeded);
        for (auto& buf : buffers) buf.resize(CLUSTER_SIZE, 0);
        
        uint64_t remaining = fileSize;
        for (size_t i = 0; i < clustersNeeded && remaining > 0; i++) {
            size_t toRead = min((uint64_t)CLUSTER_SIZE, remaining);
            fread(buffers[i].data(), 1, toRead, hostFile);
            remaining -= toRead;
        }
        fclose(hostFile);
        
        atomic<uint64_t> bytesWritten(0);
        mutex progressMutex;
        
        auto writeCluster = [&](size_t idx) {
            uint64_t cluster = clusterChain[idx];
            for (int s = 0; s < 8; s++) {
                disk.writeSector(cluster * 8 + s, buffers[idx].data() + s * SECTOR_SIZE);
            }
            size_t written = min((uint64_t)CLUSTER_SIZE, fileSize - idx * CLUSTER_SIZE);
            bytesWritten += written;
            
            lock_guard<mutex> lock(progressMutex);
            progress.update(bytesWritten, written);
        };
        
        vector<future<void>> futures;
        for (size_t i = 0; i < clusterChain.size(); i++) {
            futures.push_back(threadPool.enqueue(writeCluster, i));
        }
        
        for (auto& f : futures) {
            f.wait();
        }
        
        journal->commitOperation(txId);
        progress.finish();
        lout << "Imported " << fileSize << " bytes as '" << name;
        if (!ext.empty()) lout << "." << ext;
        lout << "' (parallel, " << clusterChain.size() << " clusters)\n";
    }
    
    void exportFile(const string& lfsPath, const string& hostPath) {
        if (!disk.isOpen()) { lout << "Not mounted.\n"; return; }
        
        PathResult res = resolvePath(lfsPath);
        if (!res.valid) { lout << "File not found.\n"; return; }
        
        DirEntry foundEntry;
        bool found = false;
        vector<uint64_t> chain = getChain(res.parentCluster);
        
        for (uint64_t c : chain) {
            for (int i = 0; i < 8; i++) {
                uint8_t sec[SECTOR_SIZE];
                disk.readSector(c * 8 + i, sec);
                DirEntry* des = (DirEntry*)sec;
                for (int j = 0; j < SECTOR_SIZE / sizeof(DirEntry); j++) {
                    if (des[j].type == TYPE_FILE && string(des[j].name) == res.name) {
                        foundEntry = des[j];
                        found = true;
                        goto export_found;
                    }
                }
            }
        }
        
export_found:
        if (!found) { lout << "File not found: " << lfsPath << "\n"; return; }
        if (!(foundEntry.attributes & PERM_READ)) {
            lout << "Permission denied: no read access.\n";
            return;
        }
        
        FILE* hostFile = fopen(hostPath.c_str(), "wb");
        if (!hostFile) {
            lout << "Cannot create host file: " << hostPath << "\n";
            return;
        }
        
        vector<uint64_t> fileChain = getChain(foundEntry.startCluster);
        uint64_t remaining = foundEntry.size;
        uint64_t total = foundEntry.size;
        uint8_t buffer[CLUSTER_SIZE];
        
        TransferProgress progress(total, "Export");
        
        for (uint64_t c : fileChain) {
            if (remaining == 0) break;
            
            for (int i = 0; i < 8; i++) {
                disk.readSector(c * 8 + i, buffer + (i * SECTOR_SIZE));
            }
            
            size_t toWrite = min((uint64_t)CLUSTER_SIZE, remaining);
            fwrite(buffer, 1, toWrite, hostFile);
            remaining -= toWrite;
            progress.update(total - remaining, toWrite);
        }
        
        fclose(hostFile);
        progress.finish();
        lout << "Exported " << foundEntry.size << " bytes to '" << hostPath << "'\n";
    }
    
    void executeFile(const string& path, const string& args) {
        if (!disk.isOpen()) { lout << "Not mounted.\n"; return; }
        
        PathResult res = resolvePath(path);
        if (!res.valid) { lout << "File not found: " << path << "\n"; return; }
        string searchName = res.name;
        string searchExt = "";
        size_t dotPos = res.name.find_last_of('.');
        if (dotPos != string::npos && dotPos > 0) {
            searchName = res.name.substr(0, dotPos);
            searchExt = res.name.substr(dotPos + 1);
        }
        
        DirEntry foundEntry;
        bool found = false;
        vector<uint64_t> chain = getChain(res.parentCluster);
        
        for (uint64_t c : chain) {
            for (int i = 0; i < 8; i++) {
                uint8_t sec[SECTOR_SIZE];
                disk.readSector(c * 8 + i, sec);
                DirEntry* des = (DirEntry*)sec;
                for (int j = 0; j < SECTOR_SIZE / sizeof(DirEntry); j++) {
                    if (des[j].type == TYPE_FILE) {
                        bool nameMatch = (string(des[j].name) == searchName);
                        bool extMatch = searchExt.empty() || (string(des[j].extension) == searchExt);
                        if (nameMatch && extMatch) {
                            foundEntry = des[j];
                            found = true;
                            goto exec_found;
                        }
                    }
                }
            }
        }
        
exec_found:
        if (!found) { lout << "File not found: " << path << "\n"; return; }
        if (!(foundEntry.attributes & PERM_EXEC)) {
            lout << "Permission denied: no execute permission.\n";
            return;
        }
        
        char tempPath[MAX_PATH];
        GetTempPathA(MAX_PATH, tempPath);
        string tempFile = string(tempPath) + "lfs_exec_" + foundEntry.name;
        if (foundEntry.extension[0] != '\0') {
            tempFile += ".";
            tempFile += foundEntry.extension;
        }
        
        FILE* outFile = fopen(tempFile.c_str(), "wb");
        if (!outFile) {
            lout << "Cannot create temp file.\n";
            return;
        }
        
        vector<uint64_t> fileChain = getChain(foundEntry.startCluster);
        uint64_t remaining = foundEntry.size;
        uint8_t buffer[CLUSTER_SIZE];
        
        for (uint64_t c : fileChain) {
            if (remaining == 0) break;
            
            for (int i = 0; i < 8; i++) {
                disk.readSector(c * 8 + i, buffer + (i * SECTOR_SIZE));
            }
            
            size_t toWrite = min((uint64_t)CLUSTER_SIZE, remaining);
            fwrite(buffer, 1, toWrite, outFile);
            remaining -= toWrite;
        }
        fclose(outFile);
        
        string cmdLine = "\"" + tempFile + "\"";
        if (!args.empty()) cmdLine += " " + args;
        
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        ZeroMemory(&pi, sizeof(pi));
        
        char cmdBuf[4096];
        strncpy(cmdBuf, cmdLine.c_str(), sizeof(cmdBuf) - 1);
        
        if (CreateProcessA(NULL, cmdBuf, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            
            DWORD exitCode;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            
            if (exitCode != 0) {
                lout << "[Exit code: " << exitCode << "]\n";
            }
        } else {
            lout << "Failed to execute. Error: " << GetLastError() << "\n";
        }
        
        DeleteFileA(tempFile.c_str());
    }
    
    uint64_t getCurrentLevelID() { return context.currentLevelID; }
    string getCurrentPath() { return context.currentPath; }
    string getCurrentVersion() { return context.currentVersion; }
    
    bool isRootFSInitialized() {
        if (!disk.isOpen()) return false;
        if (context.rootContentCluster == 0) return false;
        
        bool hasLocal = false, hasData = false;
        vector<DirEntry> entries = readDirEntries(context.rootContentCluster);
        
        for (const auto& e : entries) {
            if (e.type == TYPE_LEVELED_DIR) {
                if (string(e.name) == "local") hasLocal = true;
                if (string(e.name) == "data") hasData = true;
            }
        }
        return hasLocal && hasData;
    }
    
    void rootfsInit() {
        if (!disk.isOpen()) return;
        
        lout << "Initializing RootFS structure...\n";
        
        NavigationContext savedCtx = context;
        context.currentContentCluster = context.rootContentCluster;
        context.currentPath = "/";
        context.currentVersion = "master";
        context.currentFolderPerms = PERM_ROOT_DEFAULT;
        
        createInCluster(context.rootContentCluster, "folder", "local", "");
        createInCluster(context.rootContentCluster, "folder", "data", "");
        
        vector<DirEntry> entries = readDirEntries(context.rootContentCluster);
        uint64_t dataCluster = 0;
        for (const auto& e : entries) {
            if (e.type == TYPE_LEVELED_DIR && string(e.name) == "data") {
                dataCluster = e.startCluster;
                break;
            }
        }
        
        if (dataCluster != 0) {
            char vpsBuf[SECTOR_SIZE];
            disk.readSector(dataCluster * 8, vpsBuf);
            VersionEntry* vps = (VersionEntry*)vpsBuf;
            
            for (int j = 0; j < SECTOR_SIZE / sizeof(VersionEntry); j++) {
                if (vps[j].isActive && string(vps[j].versionName) == "master") {
                    uint64_t dataContent = vps[j].contentTableCluster;
                    createInCluster(dataContent, "file", "var", "dat");
                    break;
                }
            }
        }
        
        context = savedCtx;
        lout << "RootFS initialized: /local, /data, /data/var.dat\n";
    }
    
    void loadVariables() {
        if (!disk.isOpen()) return;
        variables.clear();
        
        vector<DirEntry> rootEntries = readDirEntries(context.rootContentCluster);
        uint64_t dataCluster = 0;
        for (const auto& e : rootEntries) {
            if (e.type == TYPE_LEVELED_DIR && string(e.name) == "data") {
                dataCluster = e.startCluster;
                break;
            }
        }
        if (dataCluster == 0) return;
        
        char vpsBuf[SECTOR_SIZE];
        disk.readSector(dataCluster * 8, vpsBuf);
        VersionEntry* vps = (VersionEntry*)vpsBuf;
        uint64_t dataContent = 0;
        
        for (int j = 0; j < SECTOR_SIZE / sizeof(VersionEntry); j++) {
            if (vps[j].isActive && string(vps[j].versionName) == "master") {
                dataContent = vps[j].contentTableCluster;
                break;
            }
        }
        if (dataContent == 0) return;
        
        vector<DirEntry> dataEntries = readDirEntries(dataContent);
        DirEntry varEntry;
        bool found = false;
        for (const auto& e : dataEntries) {
            if (e.type == TYPE_FILE && string(e.name) == "var") {
                varEntry = e;
                found = true;
                break;
            }
        }
        if (!found || varEntry.size == 0) return;
        
        vector<uint64_t> chain = getChain(varEntry.startCluster);
        string content;
        uint64_t remaining = varEntry.size;
        
        for (uint64_t c : chain) {
            if (remaining == 0) break;
            char buffer[CLUSTER_SIZE];
            for (int i = 0; i < 8; i++) {
                disk.readSector(c * 8 + i, buffer + i * SECTOR_SIZE);
            }
            size_t toRead = min((uint64_t)CLUSTER_SIZE, remaining);
            content.append(buffer, toRead);
            remaining -= toRead;
        }
        
        istringstream iss(content);
        string line;
        while (getline(iss, line)) {
            size_t eq = line.find('=');
            if (eq != string::npos) {
                string name = line.substr(0, eq);
                string value = line.substr(eq + 1);
                if (value.front() == '"' && value.back() == '"') {
                    value = value.substr(1, value.length() - 2);
                }
                variables[name] = value;
            }
        }
    }
    
    void declareVariable(const string& name, const string& value) {
        variables[name] = value;
        
        vector<DirEntry> rootEntries = readDirEntries(context.rootContentCluster);
        uint64_t dataCluster = 0;
        for (const auto& e : rootEntries) {
            if (e.type == TYPE_LEVELED_DIR && string(e.name) == "data") {
                dataCluster = e.startCluster;
                break;
            }
        }
        if (dataCluster == 0) { lout << "Error: /data not found\n"; return; }
        
        char vpsBuf[SECTOR_SIZE];
        disk.readSector(dataCluster * 8, vpsBuf);
        VersionEntry* vps = (VersionEntry*)vpsBuf;
        uint64_t dataContent = 0;
        
        for (int j = 0; j < SECTOR_SIZE / sizeof(VersionEntry); j++) {
            if (vps[j].isActive && string(vps[j].versionName) == "master") {
                dataContent = vps[j].contentTableCluster;
                break;
            }
        }
        if (dataContent == 0) { lout << "Error: /data:master not found\n"; return; }
        
        string varContent;
        for (const auto& [k, v] : variables) {
            varContent += k + "=\"" + v + "\"\n";
        }
        
        vector<DirEntry> dataEntries = readDirEntries(dataContent);
        for (uint64_t c : getChain(dataContent)) {
            for (int i = 0; i < 8; i++) {
                uint8_t sec[SECTOR_SIZE];
                disk.readSector(c * 8 + i, sec);
                DirEntry* des = (DirEntry*)sec;
                for (int j = 0; j < SECTOR_SIZE / sizeof(DirEntry); j++) {
                    if (des[j].type == TYPE_FILE && string(des[j].name) == "var") {
                        uint64_t fileCluster = des[j].startCluster;
                        if (fileCluster == 0) {
                            fileCluster = allocCluster();
                            des[j].startCluster = fileCluster;
                        }
                        
                        des[j].size = varContent.size();
                        des[j].modTime = time(0);
                        disk.writeSector(c * 8 + i, sec);
                        
                        uint8_t buffer[CLUSTER_SIZE];
                        memset(buffer, 0, CLUSTER_SIZE);
                        memcpy(buffer, varContent.c_str(), min(varContent.size(), (size_t)CLUSTER_SIZE));
                        for (int s = 0; s < 8; s++) {
                            disk.writeSector(fileCluster * 8 + s, buffer + s * SECTOR_SIZE);
                        }
                        setLATEntry(fileCluster, LAT_END);
                        
                        lout << "Variable set: " << name << "=" << value << "\n";
                        return;
                    }
                }
            }
        }
        lout << "Error: var.dat not found\n";
    }
    
    string expandVariables(const string& input) {
        string result = input;
        size_t pos = 0;
        while ((pos = result.find('$', pos)) != string::npos) {
            size_t end = pos + 1;
            while (end < result.length() && (isalnum(result[end]) || result[end] == '_')) {
                end++;
            }
            string varName = result.substr(pos + 1, end - pos - 1);
            if (!varName.empty() && variables.count(varName)) {
                result.replace(pos, end - pos, variables[varName]);
                pos += variables[varName].length();
            } else {
                pos++;
            }
        }
        return result;
    }
    
    bool tryExecuteFromLocal(const string& cmd, const string& args) {
        vector<DirEntry> rootEntries = readDirEntries(context.rootContentCluster);
        uint64_t localCluster = 0;
        for (const auto& e : rootEntries) {
            if (e.type == TYPE_LEVELED_DIR && string(e.name) == "local") {
                localCluster = e.startCluster;
                break;
            }
        }
        if (localCluster == 0) return false;
        
        char vpsBuf[SECTOR_SIZE];
        disk.readSector(localCluster * 8, vpsBuf);
        VersionEntry* vps = (VersionEntry*)vpsBuf;
        uint64_t localContent = 0;
        
        for (int j = 0; j < SECTOR_SIZE / sizeof(VersionEntry); j++) {
            if (vps[j].isActive && string(vps[j].versionName) == "master") {
                localContent = vps[j].contentTableCluster;
                break;
            }
        }
        if (localContent == 0) return false;
        
        vector<DirEntry> localEntries = readDirEntries(localContent);
        for (const auto& e : localEntries) {
            if (e.type == TYPE_FILE && string(e.name) == cmd) {
                if (!(e.attributes & PERM_EXEC)) {
                    lout << cmd << ": permission denied (no +x)\n";
                    return true;
                }
                
                NavigationContext savedCtx = context;
                context.currentContentCluster = localContent;
                executeFile(cmd, args);
                context = savedCtx;
                return true;
            }
        }
        return false;
    }
};

int main(int argc, char** argv) {
    SetConsoleOutputCP(65001); // Enable UTF-8 output
    FileSystemShell fs;
    string input;
    
    // Welcome Screen
    lout << "==========================================\n";
    lout << "      Welcome to Linuxify LevelFS        \n";
    lout << "==========================================\n";
    lout << "Type 'help' for commands.\n";
    lout << "Type 'log on' to see disk operations.\n\n";
    

    
    if (argc > 1) {
        string arg = argv[1];
        if (arg == "auto") {
            if (!fs.mountAuto()) return 1;
        } else if (arg.length() == 1 && isalpha(arg[0])) {
            if (!fs.mount(toupper(arg[0]))) return 1;
        } else {
            lout << "Error: Invalid argument '" << arg << "'. Use a drive letter (e.g., D) or 'auto'.\n";
            return 1;
        }
    } else {
        lout << "Usage: mount.exe <DriveLetter|auto>\n";
        return 1;
    }

    while (true) {
        lout << "\n";
        try {
            if (fs.isMounted()) {
                lout << "fs:" << fs.getCurrentPath() << ":" << fs.getCurrentVersion() << "$ " << flush;
            } else {
                lout << "fs> " << flush;
            }
            
            if (!getline(cin, input)) break;
            if (input.empty()) continue;

        lout << "\n";

            bool runInBackground = false;
            string trimmedInput = input;
            while (!trimmedInput.empty() && isspace(trimmedInput.back())) trimmedInput.pop_back();
            if (!trimmedInput.empty() && trimmedInput.back() == '&') {
                runInBackground = true;
                trimmedInput.pop_back();
                while (!trimmedInput.empty() && isspace(trimmedInput.back())) trimmedInput.pop_back();
            }

            stringstream ss(trimmedInput);
            string cmd;
            ss >> cmd;

            if (cmd == "exit") break;
            if (cmd == "jobs") { fs.listBackgroundTasks(); continue; }
            if (cmd == "mount") {
                string arg; ss >> arg;
                if (arg == "auto") {
                    fs.mountAuto();
                } else if (arg.length() == 1 && isalpha(arg[0])) {
                    fs.mount(toupper(arg[0]));
                } else if (arg.empty()) {
                    lout << "Usage: mount <DriveLetter|auto>\n";
                } else {
                    lout << "Error: Invalid argument '" << arg << "'. Use a drive letter (e.g., D) or 'auto'.\n";
                }
            }

            else if (cmd == "log") {
                string state; ss >> state;
                if (state == "on") fs.setVerbose(true);
                else if (state == "off") fs.setVerbose(false);
                else lout << "Usage: log <on|off>\n";
            }
            else if (cmd == "look") {
                string arg; ss >> arg;
                if (arg == "-d") {
                    string path; ss >> path;
                    fs.lookDetailed(path);
                } else {
                    fs.look(arg);
                }
            }
            else if (cmd == "perms") {
                string option, path;
                ss >> option >> path;
                if (option.empty() || path.empty()) {
                    lout << "Usage: perms <+r|-r|+w|-w|+x|-x> <path>\n";
                } else {
                    fs.perms(option, path);
                }
            }
            else if (cmd == "dir-tree") fs.dirTree();
            else if (cmd == "create") {
                string type, fullName;
                ss >> type >> fullName;
                string name = fullName, ext = "";
                size_t dot = fullName.find_last_of('.');
                if (dot != string::npos && dot > 0) {
                    name = fullName.substr(0, dot);
                    ext = fullName.substr(dot + 1);
                }
                fs.create(type, name, ext);
            }
            else if (cmd == "nav") {
                string path; ss >> path;
                fs.nav(path);
            }
            else if (cmd == "read") {
                string file; ss >> file;
                fs.read(file);
            }
            else if (cmd == "del") {
                string arg1, arg2;
                ss >> arg1 >> arg2;
                if (arg1 == "-r") {
                    fs.del(arg2, true);
                } else {
                    fs.del(arg1, false);
                }
            }
            else if (cmd == "move") {
                string src, dst;
                ss >> src >> dst;
                fs.move(src, dst);
            }
            else if (cmd == "level") {
                string sub, arg1, arg2, arg3;
                ss >> sub;
                if (sub == "add") {
                    ss >> arg1 >> arg2;
                    if (!arg1.empty() && !arg2.empty()) fs.levelAdd(arg1, arg2);
                    else lout << "Usage: level add <folder|.> <levelname>\n";
                }
                else if (sub == "branch") {
                    ss >> arg1 >> arg2 >> arg3;
                    if (!arg1.empty() && !arg2.empty() && !arg3.empty()) fs.levelBranch(arg1, arg2, arg3);
                    else lout << "Usage: level branch <folder|.> <parent_level> <new_level>\n";
                }
                else if (sub == "remove") {
                    ss >> arg1 >> arg2;
                    if (!arg1.empty() && !arg2.empty()) fs.levelRemove(arg1, arg2);
                    else lout << "Usage: level remove <folder|.> <levelname>\n";
                }
                else if (sub == "rename") {
                    ss >> arg1 >> arg2 >> arg3;
                    if (!arg1.empty() && !arg2.empty() && !arg3.empty()) fs.levelRename(arg1, arg2, arg3);
                    else lout << "Usage: level rename <folder|.> <old> <new>\n";
                }
            }
            else if (cmd == "link") {
                string dir1, dir2, levelName;
                ss >> dir1 >> dir2 >> levelName;
                if (dir1.empty() || dir2.empty() || levelName.empty()) {
                    lout << "Usage: link <dir1> <dir2> <shared_level_name>\n";
                } else {
                    fs.linkLevel(dir1, dir2, levelName);
                }
            }
            else if (cmd == "mount-level") {
                string path, levelIdStr;
                ss >> path >> levelIdStr;
                if (path.empty() || levelIdStr.empty()) {
                    lout << "Usage: mount-level <path> <levelID>\n";
                } else {
                    fs.createLevelMount(path, stoull(levelIdStr));
                }
            }
            else if (cmd == "current") fs.current();
            else if (cmd == "levels") fs.listAllLevels();
            else if (cmd == "symlink") {
                string target, link;
                ss >> target >> link;
                if (target.empty() || link.empty()) lout << "Usage: symlink <target> <linkname>\n";
                else fs.createSymlink(link, target);
            }
            else if (cmd == "hardlink") {
                string target, link;
                ss >> target >> link;
                if (target.empty() || link.empty()) lout << "Usage: hardlink <target> <linkname>\n";
                else fs.createHardlink(link, target);
            }
            else if (cmd == "write") {
                string sub, file;
                ss >> sub;
                if (sub == "insert") {
                    ss >> file;
                    if (file.empty()) lout << "Usage: write insert <filename>\n";
                    else fs.writeInsert(file);
                } else if (!sub.empty()) {
                    fs.write(sub);
                } else {
                    lout << "Usage: write <filename> or write insert <filename>\n";
                }
            }
            else if (cmd == "rename") {
                string path, newName;
                ss >> path >> newName;
                if (path.empty() || newName.empty()) {
                    lout << "Usage: rename <path|folder:level> <newname>\n";
                } else {
                    fs.rename(path, newName);
                }
            }
            else if (cmd == "help") {
                lout << "Commands:\n";
                lout << "  mount <D|auto> - Mount drive letter or auto-scan\n";
                lout << "  log <on|off>  - Toggle disk op logging\n";
                lout << "  look          - List directory contents\n";
                lout << "  look <folder> - List levels of a folder\n";
                lout << "  look <f>:<l>  - List contents of folder:level\n";
                lout << "  look -d [path]- Detailed view (size, perms, time)\n";
                lout << "  dir-tree      - Display directory tree\n";
                lout << "  current       - Show current path and level\n";
                lout << "  levels        - List all levels in registry\n";
                lout << "  create folder <name> - Create folder\n";
                lout << "  create file <name.ext> - Create file (e.g. readme.txt)\n";
                lout << "  write <name.ext>  - Text editor for file\n";
                lout << "  write insert <name.ext> - Insert line at position (arrow keys)\n";
                lout << "  read <name.ext>   - Read file contents\n";
                lout << "  rename <path> <newname> - Rename file/folder/level\n";
                lout << "  perms <+/-rwx> <file> - Set permissions (+r,-w,+x...)\n";
                lout << "  symlink <target> <link> - Create symbolic link\n";
                lout << "  hardlink <target> <link> - Create hard link\n";
                lout << "  mount-level <path> <id> - Mount level by ID at path\n";
                lout << "  nav <path>    - Navigate to folder\n";
                lout << "  del <name>    - Delete entry (links preserved)\n";
                lout << "  move <s> <d>  - Move/rename entry\n";
                lout << "  level add <f> <n>    - Add level to folder/.\n";
                lout << "  level branch <f> <p> <n> - Branch level from parent\n";
                lout << "  level remove <f> <n> - Remove level from folder/.\n";
                lout << "  link <dir1> <dir2> <level> - Create shared level (DAG)\n";
                lout << "  import <host-path> [name.ext] - Import file from host\n";
                lout << "  export <name.ext> <host> - Export file to host\n";
                lout << "./<name.ext>      - Execute binary (requires +x)\n";
                lout << "  fsck          - Check filesystem integrity\n";
                lout << "  fraginfo      - Show fragmentation info\n";
                lout << "  defrag [-nvfr] - Defragment disk\n";
                lout << "                  -n/--dry-run: analyze only\n";
                lout << "                  -v/--verbose: detailed output\n";
                lout << "                  -f/--force: force processing\n";
                lout << "                  -r/--recursive: include subdirs\n";
                lout << "  declare <n>=<v> - Set variable (use $n to expand)\n";
                lout << "  jobs          - List background tasks\n";
                lout << "  <command> &   - Run command in background\n";
                lout << "  exit          - Exit\n";
            }
            else if (cmd == "fsck") fs.fsck();
            else if (cmd == "fraginfo") fs.fragInfo();
            else if (cmd == "defrag") {
                string flags;
                getline(ss, flags);
                if (flags.empty()) fs.defrag();
                else fs.defragWithFlags(flags);
            }
            else if (cmd == "import") {
                string rest;
                getline(ss, rest);
                if (rest.empty() || (rest.length() == 1 && rest[0] == ' ')) {
                    lout << "Usage: import <host-path> [lfs-name]\n";
                } else {
                    if (rest[0] == ' ') rest = rest.substr(1);
                    string hostPath, lfsName;
                    if (rest[0] == '"') {
                        size_t endQuote = rest.find('"', 1);
                        if (endQuote != string::npos) {
                            hostPath = rest.substr(1, endQuote - 1);
                            string remaining = rest.substr(endQuote + 1);
                            stringstream rem(remaining);
                            rem >> lfsName;
                        } else {
                            hostPath = rest;
                        }
                    } else {
                        stringstream argss(rest);
                        argss >> hostPath >> lfsName;
                    }
                    fs.importFile(hostPath, lfsName);
                }
            }
            else if (cmd == "export") {
                string lfsPath, hostPath;
                ss >> lfsPath >> hostPath;
                if (lfsPath.empty() || hostPath.empty()) {
                    lout << "Usage: export <lfs-path> <host-path>\n";
                } else {
                    fs.exportFile(lfsPath, hostPath);
                }
            }
            else if (cmd.length() >= 2 && cmd.substr(0, 2) == "./") {
                string binary = cmd.substr(2);
                string args;
                getline(ss, args);
                if (!args.empty() && args[0] == ' ') args = args.substr(1);
                args = fs.expandVariables(args);
                fs.executeFile(binary, args);
            }
            else if (cmd == "declare") {
                string rest;
                getline(ss, rest);
                if (!rest.empty() && rest[0] == ' ') rest = rest.substr(1);
                size_t eq = rest.find('=');
                if (eq == string::npos) {
                    lout << "Usage: declare <name>=<value> or declare <name>=\"string\"\n";
                } else {
                    string name = rest.substr(0, eq);
                    string value = rest.substr(eq + 1);
                    if (value.front() == '"' && value.back() == '"') {
                        value = value.substr(1, value.length() - 2);
                    }
                    fs.declareVariable(name, value);
                }
            }
            else {
                string args;
                getline(ss, args);
                if (!args.empty() && args[0] == ' ') args = args.substr(1);
                args = fs.expandVariables(args);
                if (!fs.tryExecuteFromLocal(cmd, args)) {
                    lout << "Unknown command. Type 'help' for list.\n";
                }
            }
        } catch (const exception& e) {
            lout << "Error: " << e.what() << "\n";
        } catch (...) {
            lout << "Unknown error occurred.\n";
        }
    }
    return 0;
}
