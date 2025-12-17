/*
 * defrag.hpp - Disk Defragmentation for LevelFS
 * 
 * Compile: Include in mount.cpp with #include "defrag.hpp"
 * 
 * Usage:
 *   Defragmenter defrag(disk, sb);
 *   defrag.run(contentCluster, flags);
 * 
 * Flags:
 *   DEFRAG_DRY_RUN     - Analyze only, don't move data
 *   DEFRAG_VERBOSE     - Show detailed progress
 *   DEFRAG_FORCE       - Process files even if risky (no write perms)
 *   DEFRAG_RECURSIVE   - Defragment all subdirectories
 */

#ifndef DEFRAG_HPP
#define DEFRAG_HPP

#include "fs_common.hpp"
#include <vector>
#include <string>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cstring>

using namespace std;

enum DefragFlags {
    DEFRAG_NONE      = 0,
    DEFRAG_DRY_RUN   = 1 << 0,
    DEFRAG_VERBOSE   = 1 << 1,
    DEFRAG_FORCE     = 1 << 2,
    DEFRAG_RECURSIVE = 1 << 3
};

struct DefragResult {
    int filesProcessed;
    int filesSkipped;
    int filesFailed;
    int foldersProcessed;
    int clustersRecovered;
    uint64_t bytesProcessed;
    bool success;
    string errorMessage;
    
    DefragResult() : filesProcessed(0), filesSkipped(0), filesFailed(0),
                     foldersProcessed(0), clustersRecovered(0), 
                     bytesProcessed(0), success(true) {}
    
    void merge(const DefragResult& other) {
        filesProcessed += other.filesProcessed;
        filesSkipped += other.filesSkipped;
        filesFailed += other.filesFailed;
        foldersProcessed += other.foldersProcessed;
        clustersRecovered += other.clustersRecovered;
        bytesProcessed += other.bytesProcessed;
        if (!other.success) success = false;
        if (!other.errorMessage.empty()) errorMessage = other.errorMessage;
    }
};

struct FragmentInfo {
    string name;
    string path;
    uint64_t startCluster;
    uint64_t size;
    int fragmentCount;
    int clusterCount;
    bool isContiguous;
    bool hasWritePermission;
};

struct DefragProgress {
    int totalFiles;
    int currentFile;
    int currentPercent;
    
    DefragProgress() : totalFiles(0), currentFile(0), currentPercent(0) {}
    
    void update(int current, int total) {
        currentFile = current;
        totalFiles = total;
        if (total > 0) {
            currentPercent = (current * 100) / total;
        }
    }
    
    void display() {
        cout << "\r  Progress: [";
        int barWidth = 30;
        int filled = (currentPercent * barWidth) / 100;
        for (int i = 0; i < barWidth; i++) {
            if (i < filled) cout << "=";
            else if (i == filled) cout << ">";
            else cout << " ";
        }
        cout << "] " << setw(3) << currentPercent << "% (" 
             << currentFile << "/" << totalFiles << ")";
        cout.flush();
    }
    
    void complete() {
        cout << "\r  Progress: [" << string(30, '=') << "] 100%";
        cout << string(20, ' ') << "\n";
    }
};

class Defragmenter {
private:
    DiskDevice& disk;
    SuperBlock& sb;
    int flags;
    DefragResult result;
    DefragProgress progress;
    int depth;
    
    uint64_t getDataStart() {
        return sb.labPoolStart + sb.labPoolClusters;
    }
    
    LABEntry getLABEntry(uint64_t cluster) {
        LABEntry buffer[LAB_ENTRIES_PER_CLUSTER];
        uint64_t sector = cluster / LAB_ENTRIES_PER_CLUSTER;
        uint64_t offset = cluster % LAB_ENTRIES_PER_CLUSTER;
        disk.readSector(sector, buffer);
        return buffer[offset];
    }
    
    void setLABEntry(uint64_t cluster, uint64_t nextCluster) {
        LABEntry buffer[LAB_ENTRIES_PER_CLUSTER];
        uint64_t sector = cluster / LAB_ENTRIES_PER_CLUSTER;
        uint64_t offset = cluster % LAB_ENTRIES_PER_CLUSTER;
        disk.readSector(sector, buffer);
        buffer[offset].nextCluster = nextCluster;
        disk.writeSector(sector, buffer);
    }
    
    vector<uint64_t> getChain(uint64_t startCluster) {
        vector<uint64_t> chain;
        if (startCluster == 0) return chain;
        
        uint64_t current = startCluster;
        int limit = 100000;
        
        while (current != 0 && current != LAT_END && current != LAT_FREE && limit-- > 0) {
            chain.push_back(current);
            LABEntry lab = getLABEntry(current);
            if (lab.nextCluster == current) break;
            current = lab.nextCluster;
        }
        return chain;
    }
    
    string getIndent() {
        return string(depth * 2, ' ');
    }
    
    FragmentInfo analyzeFile(const DirEntry& entry, const string& currentPath = "") {
        FragmentInfo info;
        char nameBuf[25];
        strncpy(nameBuf, entry.name, 24);
        nameBuf[24] = '\0';
        info.name = nameBuf;
        if (entry.extension[0]) {
            info.name += ".";
            info.name += entry.extension;
        }
        info.path = currentPath + "/" + info.name;
        
        info.startCluster = entry.startCluster;
        info.size = entry.size;
        info.hasWritePermission = (entry.attributes & PERM_WRITE) != 0;
        
        vector<uint64_t> chain = getChain(entry.startCluster);
        info.clusterCount = chain.size();
        
        info.fragmentCount = 1;
        for (size_t i = 1; i < chain.size(); i++) {
            if (chain[i] != chain[i-1] + 1) info.fragmentCount++;
        }
        
        info.isContiguous = (info.fragmentCount <= 1);
        return info;
    }
    
    uint64_t findContiguousSpace(uint64_t needed) {
        uint64_t dataStart = getDataStart();
        uint64_t consecutive = 0;
        uint64_t candidateStart = 0;
        
        for (uint64_t c = dataStart; c < sb.totalClusters; c++) {
            LABEntry lab = getLABEntry(c);
            if (lab.nextCluster == LAT_FREE) {
                if (consecutive == 0) candidateStart = c;
                consecutive++;
                if (consecutive >= needed) return candidateStart;
            } else {
                consecutive = 0;
            }
        }
        return 0;
    }
    
    bool verifyClusterData(uint64_t cluster, const uint8_t* expectedData) {
        uint8_t buffer[CLUSTER_SIZE];
        for (int s = 0; s < 8; s++) {
            if (!disk.readSector(cluster * 8 + s, buffer + s * SECTOR_SIZE)) {
                return false;
            }
        }
        return memcmp(buffer, expectedData, CLUSTER_SIZE) == 0;
    }
    
    bool moveClusterChain(const vector<uint64_t>& oldChain, uint64_t newStart, 
                          uint64_t entrySector, int entryIdx) {
        if (oldChain.empty()) return true;
        
        vector<uint8_t> backupData(oldChain.size() * CLUSTER_SIZE);
        
        for (size_t i = 0; i < oldChain.size(); i++) {
            uint8_t* buffer = backupData.data() + i * CLUSTER_SIZE;
            
            for (int s = 0; s < 8; s++) {
                if (!disk.readSector(oldChain[i] * 8 + s, buffer + s * SECTOR_SIZE)) {
                    result.errorMessage = "Failed to read cluster " + to_string(oldChain[i]);
                    return false;
                }
            }
        }
        
        for (size_t i = 0; i < oldChain.size(); i++) {
            uint8_t* buffer = backupData.data() + i * CLUSTER_SIZE;
            
            for (int s = 0; s < 8; s++) {
                if (!disk.writeSector((newStart + i) * 8 + s, buffer + s * SECTOR_SIZE)) {
                    result.errorMessage = "Failed to write cluster " + to_string(newStart + i);
                    return false;
                }
            }
            
            if (!verifyClusterData(newStart + i, buffer)) {
                result.errorMessage = "Data verification failed for cluster " + to_string(newStart + i);
                return false;
            }
        }
        
        DirEntry entries[SECTOR_SIZE / sizeof(DirEntry)];
        if (!disk.readSector(entrySector, entries)) {
            result.errorMessage = "Failed to read directory entry";
            return false;
        }
        uint64_t oldStart = entries[entryIdx].startCluster;
        entries[entryIdx].startCluster = newStart;
        if (!disk.writeSector(entrySector, entries)) {
            result.errorMessage = "Failed to update directory entry";
            return false;
        }
        
        for (size_t i = 0; i < oldChain.size(); i++) {
            if (i < oldChain.size() - 1) {
                setLABEntry(newStart + i, newStart + i + 1);
            } else {
                setLABEntry(newStart + i, LAT_END);
            }
        }
        
        for (uint64_t c : oldChain) {
            setLABEntry(c, LAT_FREE);
        }
        
        return true;
    }
    
    bool defragFile(const DirEntry& entry, uint64_t entrySector, int entryIdx, 
                    const string& currentPath = "") {
        if (entry.type != TYPE_FILE || entry.startCluster == 0) return true;
        
        FragmentInfo info = analyzeFile(entry, currentPath);
        
        if (!info.hasWritePermission && !(flags & DEFRAG_FORCE)) {
            if (flags & DEFRAG_VERBOSE) {
                cout << getIndent() << "  SKIP " << info.name << " (no write permission, use -f)\n";
            }
            result.filesSkipped++;
            return true;
        }
        
        if (info.isContiguous) {
            if (flags & DEFRAG_VERBOSE) {
                cout << getIndent() << "  SKIP " << info.name << " (already contiguous)\n";
            }
            result.filesSkipped++;
            return true;
        }
        
        vector<uint64_t> oldChain = getChain(entry.startCluster);
        uint64_t needed = oldChain.size();
        
        if (flags & DEFRAG_DRY_RUN) {
            cout << getIndent() << "  [DRY-RUN] Would defragment " << info.name 
                 << " (" << info.fragmentCount << " fragments -> 1)\n";
            result.filesProcessed++;
            return true;
        }
        
        uint64_t newStart = findContiguousSpace(needed);
        if (newStart == 0) {
            if (flags & DEFRAG_VERBOSE) {
                cout << getIndent() << "  FAIL " << info.name 
                     << " (no contiguous space for " << needed << " clusters)\n";
            }
            result.filesFailed++;
            result.errorMessage = "No contiguous space for " + info.name;
            return false;
        }
        
        if (flags & DEFRAG_VERBOSE) {
            cout << getIndent() << "  MOVE " << info.name << " (" << info.fragmentCount << " frags) "
                 << "clusters " << entry.startCluster << " -> " << newStart << "... ";
            cout.flush();
        }
        
        if (!moveClusterChain(oldChain, newStart, entrySector, entryIdx)) {
            if (flags & DEFRAG_VERBOSE) cout << "FAILED\n";
            result.filesFailed++;
            result.success = false;
            return false;
        }
        
        if (flags & DEFRAG_VERBOSE) cout << "OK\n";
        result.filesProcessed++;
        result.clustersRecovered += info.fragmentCount - 1;
        result.bytesProcessed += entry.size;
        return true;
    }
    
    void defragDirectory(uint64_t contentCluster, const string& path) {
        vector<uint64_t> chain = getChain(contentCluster);
        vector<pair<DirEntry, pair<uint64_t, int>>> files;
        vector<pair<DirEntry, uint64_t>> folders;
        
        for (uint64_t c : chain) {
            for (int i = 0; i < 8; i++) {
                DirEntry entries[SECTOR_SIZE / sizeof(DirEntry)];
                disk.readSector(c * 8 + i, entries);
                
                for (int j = 0; j < SECTOR_SIZE / sizeof(DirEntry); j++) {
                    if (entries[j].type == TYPE_FILE && entries[j].startCluster != 0) {
                        files.push_back({entries[j], {c * 8 + i, j}});
                    }
                    else if (entries[j].type == TYPE_LEVELED_DIR && entries[j].startCluster != 0) {
                        folders.push_back({entries[j], entries[j].startCluster});
                    }
                }
            }
        }
        
        progress.totalFiles += files.size();
        
        for (size_t i = 0; i < files.size(); i++) {
            defragFile(files[i].first, files[i].second.first, files[i].second.second, path);
            progress.currentFile++;
            if (!(flags & DEFRAG_VERBOSE)) {
                progress.display();
            }
        }
        
        if ((flags & DEFRAG_RECURSIVE) && !folders.empty()) {
            for (const auto& folder : folders) {
                char nameBuf[25];
                strncpy(nameBuf, folder.first.name, 24);
                nameBuf[24] = '\0';
                string folderName = nameBuf;
                string newPath = path + "/" + folderName;
                
                if (flags & DEFRAG_VERBOSE) {
                    cout << getIndent() << "Entering folder: " << folderName << "\n";
                }
                
                depth++;
                result.foldersProcessed++;
                
                char vpsBuf[SECTOR_SIZE];
                VersionEntry* vps = (VersionEntry*)vpsBuf;
                for (int s = 0; s < 8; s++) {
                    disk.readSector(folder.second * 8 + s, vpsBuf);
                    for (int j = 0; j < SECTOR_SIZE / sizeof(VersionEntry); j++) {
                        if (vps[j].isActive && vps[j].contentTableCluster != 0) {
                            if (flags & DEFRAG_VERBOSE) {
                                cout << getIndent() << "Level: " << vps[j].versionName << "\n";
                            }
                            defragDirectory(vps[j].contentTableCluster, newPath + ":" + vps[j].versionName);
                        }
                    }
                }
                
                depth--;
            }
        }
    }
    
    int countTotalFiles(uint64_t contentCluster) {
        int count = 0;
        vector<uint64_t> chain = getChain(contentCluster);
        
        for (uint64_t c : chain) {
            for (int i = 0; i < 8; i++) {
                DirEntry entries[SECTOR_SIZE / sizeof(DirEntry)];
                disk.readSector(c * 8 + i, entries);
                
                for (int j = 0; j < SECTOR_SIZE / sizeof(DirEntry); j++) {
                    if (entries[j].type == TYPE_FILE && entries[j].startCluster != 0) {
                        count++;
                    }
                    else if ((flags & DEFRAG_RECURSIVE) && 
                             entries[j].type == TYPE_LEVELED_DIR && entries[j].startCluster != 0) {
                        char vpsBuf[SECTOR_SIZE];
                        VersionEntry* vps = (VersionEntry*)vpsBuf;
                        for (int s = 0; s < 8; s++) {
                            disk.readSector(entries[j].startCluster * 8 + s, vpsBuf);
                            for (int v = 0; v < SECTOR_SIZE / sizeof(VersionEntry); v++) {
                                if (vps[v].isActive && vps[v].contentTableCluster != 0) {
                                    count += countTotalFiles(vps[v].contentTableCluster);
                                }
                            }
                        }
                    }
                }
            }
        }
        return count;
    }

public:
    Defragmenter(DiskDevice& d, SuperBlock& s) : disk(d), sb(s), flags(DEFRAG_NONE), depth(0) {}
    
    DefragResult analyze(uint64_t contentCluster, const string& path = "/") {
        DefragResult analysisResult;
        vector<FragmentInfo> fragmented;
        vector<FragmentInfo> allFiles;
        int totalFragments = 0;
        
        vector<uint64_t> chain = getChain(contentCluster);
        for (uint64_t c : chain) {
            for (int i = 0; i < 8; i++) {
                DirEntry entries[SECTOR_SIZE / sizeof(DirEntry)];
                disk.readSector(c * 8 + i, entries);
                
                for (int j = 0; j < SECTOR_SIZE / sizeof(DirEntry); j++) {
                    if (entries[j].type == TYPE_FILE && entries[j].startCluster != 0) {
                        FragmentInfo info = analyzeFile(entries[j], path);
                        allFiles.push_back(info);
                        if (!info.isContiguous) {
                            fragmented.push_back(info);
                            totalFragments += info.fragmentCount;
                        }
                    }
                }
            }
        }
        
        cout << "\n=== Fragmentation Analysis ===\n\n";
        cout << "Path: " << path << "\n\n";
        
        if (fragmented.empty()) {
            cout << "No fragmented files found.\n";
        } else {
            cout << "Fragmented files:\n";
            cout << string(60, '-') << "\n";
            cout << setw(28) << left << "Name" 
                 << setw(10) << right << "Fragments"
                 << setw(12) << "Clusters"
                 << setw(10) << "Perms" << "\n";
            cout << string(60, '-') << "\n";
            
            for (const auto& f : fragmented) {
                cout << setw(28) << left << f.name 
                     << setw(10) << right << f.fragmentCount
                     << setw(12) << f.clusterCount
                     << setw(10) << (f.hasWritePermission ? "rw" : "r-") << "\n";
            }
            cout << string(60, '-') << "\n";
        }
        
        cout << "\nSummary:\n";
        cout << "  Total files: " << allFiles.size() << "\n";
        cout << "  Fragmented: " << fragmented.size() << "\n";
        if (!allFiles.empty()) {
            int pct = (fragmented.size() * 100) / allFiles.size();
            cout << "  Fragmentation: " << pct << "%\n";
        }
        cout << "  Total fragments: " << totalFragments << "\n";
        
        uint64_t totalSize = 0;
        for (const auto& f : fragmented) totalSize += f.size;
        cout << "  Fragmented data: " << (totalSize / 1024) << " KB\n";
        
        analysisResult.filesProcessed = fragmented.size();
        return analysisResult;
    }
    
    DefragResult run(uint64_t contentCluster, int defragFlags = DEFRAG_NONE, 
                     const string& path = "/") {
        flags = defragFlags;
        result = DefragResult();
        progress = DefragProgress();
        depth = 0;
        
        cout << "\n=== Disk Defragmentation ===\n";
        if (flags & DEFRAG_DRY_RUN) cout << "[DRY-RUN MODE]\n";
        if (flags & DEFRAG_VERBOSE) cout << "[VERBOSE MODE]\n";
        if (flags & DEFRAG_FORCE) cout << "[FORCE MODE]\n";
        if (flags & DEFRAG_RECURSIVE) cout << "[RECURSIVE MODE]\n";
        cout << "\n";
        
        cout << "Counting files...\n";
        int totalFiles = countTotalFiles(contentCluster);
        cout << "Found " << totalFiles << " files to process.\n\n";
        
        defragDirectory(contentCluster, path);
        
        if (!(flags & DEFRAG_VERBOSE) && progress.totalFiles > 0) {
            progress.complete();
        }
        
        cout << "\n=== Defragmentation Complete ===\n";
        cout << "  Files defragmented: " << result.filesProcessed << "\n";
        cout << "  Files skipped: " << result.filesSkipped << "\n";
        cout << "  Files failed: " << result.filesFailed << "\n";
        if (flags & DEFRAG_RECURSIVE) {
            cout << "  Folders processed: " << result.foldersProcessed << "\n";
        }
        cout << "  Clusters recovered: " << result.clustersRecovered << "\n";
        cout << "  Data processed: " << (result.bytesProcessed / 1024) << " KB\n";
        
        if (!result.errorMessage.empty()) {
            cout << "  Last error: " << result.errorMessage << "\n";
        }
        
        if (result.filesFailed == 0 && result.filesProcessed > 0) {
            cout << "\n  Status: SUCCESS\n";
        } else if (result.filesFailed > 0) {
            cout << "\n  Status: COMPLETED WITH ERRORS\n";
        } else {
            cout << "\n  Status: NO ACTION NEEDED\n";
        }
        
        return result;
    }
};

#endif
