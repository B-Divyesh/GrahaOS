#!/usr/bin/env python3
import struct
import sys
import hashlib

def analyze_disk(filename):
    """Analyze GrahaFS disk image"""
    BLOCK_SIZE = 4096
    
    with open(filename, 'rb') as f:
        # Read superblock
        f.seek(0)
        superblock = f.read(BLOCK_SIZE)
        
        # Parse magic (first 8 bytes)
        magic = struct.unpack('<Q', superblock[0:8])[0]
        print(f"Magic: 0x{magic:016x}")
        
        if magic != 0x47524148414F5321:
            print("ERROR: Invalid magic number!")
            print (magic)
            return
            
        # Parse superblock fields
        total_blocks = struct.unpack('<I', superblock[8:12])[0]
        bitmap_start = struct.unpack('<I', superblock[12:16])[0]
        inode_start = struct.unpack('<I', superblock[16:20])[0]
        data_start = struct.unpack('<I', superblock[20:24])[0]
        root_inode = struct.unpack('<I', superblock[24:28])[0]
        free_blocks = struct.unpack('<I', superblock[28:32])[0]
        free_inodes = struct.unpack('<I', superblock[32:36])[0]
        
        print(f"Total blocks: {total_blocks}")
        print(f"Bitmap start: {bitmap_start}")
        print(f"Inode table start: {inode_start}")
        print(f"Data blocks start: {data_start}")
        print(f"Root inode: {root_inode}")
        print(f"Free blocks: {free_blocks}")
        print(f"Free inodes: {free_inodes}")
        
        # Check bitmap
        print("\n=== Bitmap Analysis ===")
        f.seek(bitmap_start * BLOCK_SIZE)
        bitmap = f.read(BLOCK_SIZE)
        used_blocks = sum(bin(byte).count('1') for byte in bitmap)
        print(f"Used blocks (from bitmap): {used_blocks}")
        
        # Check root inode
        print("\n=== Root Inode Analysis ===")
        inode_offset = inode_start * BLOCK_SIZE + (root_inode * 128)
        f.seek(inode_offset)
        root_inode_data = f.read(128)
        
        inode_type = struct.unpack('<H', root_inode_data[0:2])[0]
        inode_size = struct.unpack('<Q', root_inode_data[16:24])[0]
        first_block = struct.unpack('<I', root_inode_data[56:60])[0]
        
        print(f"Root inode type: {inode_type} (should be 2 for directory)")
        print(f"Root directory size: {inode_size} bytes")
        print(f"First data block: {first_block}")
        
        # Check root directory entries
        if first_block > 0:
            print("\n=== Root Directory Entries ===")
            f.seek(first_block * BLOCK_SIZE)
            for i in range(10):  # Check first 10 entries
                entry_data = f.read(32)
                if len(entry_data) < 32:
                    break
                    
                entry_inode = struct.unpack('<I', entry_data[0:4])[0]
                entry_name = entry_data[4:32].split(b'\x00')[0].decode('ascii', errors='ignore')
                
                if entry_inode > 0:
                    print(f"  Entry {i}: inode={entry_inode}, name='{entry_name}'")
        
        # Calculate disk checksum
        f.seek(0)
        disk_data = f.read()
        checksum = hashlib.md5(disk_data).hexdigest()
        print(f"\nDisk MD5: {checksum}")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <disk.img>")
        sys.exit(1)
    
    analyze_disk(sys.argv[1])