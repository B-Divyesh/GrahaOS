target remote localhost:1234

break grahafs_write
break grahafs_create
break ahci_flush_cache
break ahci_write
break write_fs_block
break vfs_sync

break write_superblock

set logging file gdb_persistence.log
set logging on

define dump_superblock
    print "=== Superblock Debug ==="
    x/32xg &superblock
    print "Magic: "
    print/x superblock.magic
    print "Total blocks: "
    print superblock.total_blocks
    print "Free blocks: "
    print superblock.free_blocks
    print "Root inode: "
    print superblock.root_inode
end

define dump_ahci_state
    print "=== AHCI State ==="
    print "Port count: "
    print port_count
    print "HBA memory base: "
    print/x hba_mem
    # Check port command/status
    if ports[0] != 0
        print "Port 0 TFD: "
        x/1xw &(ports[0]->tfd)
        print "Port 0 CMD: "
        x/1xw &(ports[0]->cmd)
        print "Port 0 CI: "
        x/1xw &(ports[0]->ci)
    end
end

define trace_write_operation
    print "=== Tracing Write Operation ==="
    set $watch_block = $arg0
    watch *((uint32_t*)($watch_block))
    commands
        silent
        backtrace 2
        continue
    end
end

commands 1
    print "grahafs_write called"
    print "offset="
    print offset
    print "size="
    print size
    continue
end

commands 2
    print "grahafs_create called"
    print "name="
    print name
    print "type="
    print type
    continue
end

commands 3
    print "ahci_flush_cache called"
    print "port_num="
    print port_num
    dump_ahci_state
    continue
end

commands 4
    print "ahci_write called"
    print "lba="
    print lba
    print "count="
    print count
    continue
end

commands 5
    print "write_fs_block called"
    print "block_num="
    print block_num
    continue
end


continue