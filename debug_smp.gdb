target remote localhost:1234

break grahafs_create
break add_dirent
break allocate_block
break allocate_inode
break write_inode
break write_superblock

define check_superblock
    print "=== Checking Superblock ==="
    if superblock.magic == 0x47524148414F5321
        print "✓ Magic valid"
    else
        print "✗ Magic corrupted!"
        print/x superblock.magic
    end
    print "Free blocks:"
    print superblock.free_blocks
    print "Free inodes:"
    print superblock.free_inodes
end

define check_params
    print "=== Parameters ==="
    print "Parent node:"
    print/x parent
    if parent != 0 && parent < 0xFFFF800000000000
        print "✗ Invalid parent pointer!"
    end
    print "Name:"
    if name != 0
        x/s name
    else
        print "✗ NULL name!"
    end
    print "Type:"
    print type
end

commands 1
    check_params
    check_superblock
    continue
end

continue