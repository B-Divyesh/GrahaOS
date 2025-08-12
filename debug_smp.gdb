# Complete debugging script
target remote localhost:1234
symbol-file kernel/kernel.elf
set architecture i386:x86-64
set pagination off
set print pretty on

# Monitor keyboard task creation
break sched_create_task
commands
    printf "\n=== sched_create_task ===\n"
    printf "entry_point parameter: 0x%lx\n", entry_point
    printf "RDI register: 0x%lx\n", $rdi
    
    # Check what's at RDI
    if $rdi != 0
        x/i $rdi
    end
    
    # Let it continue if valid
    if entry_point != 0 || $rdi >= 0xFFFFFFFF80000000
        continue
    else
        echo NULL or invalid entry point - stopping\n
    end
end

# Monitor frame issues
break interrupt_handler
commands
    silent
    set $addr = (uint64_t)frame
    
    # Only print if frame looks wrong
    if $addr < 0xFFFF800000000000
        printf "\n*** BAD FRAME: 0x%lx ***\n", $addr
        printf "RSP: 0x%lx\n", $rsp
        printf "RDI: 0x%lx\n", $rdi
        bt 3
        
        # Try to see what interrupt it is
        if $addr != 0
            printf "Trying to read int_no at bad address...\n"
            # Don't actually read it as it will fail
        end
    else
        # Frame looks okay, continue
        continue  
    end
end

# Monitor keyboard task execution
break keyboard_poll_task
commands
    printf "\n*** Keyboard task started on CPU %d ***\n", smp_get_current_cpu()
    printf "RSP: 0x%lx\n", $rsp
    
    # Check if stack is valid
    if $rsp < 0xFFFF800000000000
        echo ERROR: Invalid stack pointer!\n
    else
        echo Stack looks good\n
        continue
    end
end

# Check when interrupts are enabled
break *kmain+2000
commands
    echo \n*** Near end of kmain ***\n
    continue
end

# Monitor LAPIC timer
break lapic_timer_init
commands
    printf "\n*** LAPIC timer init on CPU %d ***\n", smp_get_current_cpu()
    continue
end

break lapic_timer_stop
commands
    printf "\n*** LAPIC timer stop on CPU %d ***\n", smp_get_current_cpu()
    continue
end

# Custom commands
define check_tasks
    echo \n=== Task List ===\n
    set $i = 0
    while $i < next_task_id
        if tasks[$i].state != 0
            printf "Task %d: state=%d rip=0x%lx rsp=0x%lx\n", \
                $i, tasks[$i].state, tasks[$i].regs.rip, tasks[$i].regs.rsp
        end
        set $i = $i + 1
    end
end

define check_cpus
    echo \n=== CPU Status ===\n
    printf "CPU count: %d\n", g_cpu_count
    printf "APs started: %d\n", aps_started
    set $i = 0
    while $i < g_cpu_count
        printf "CPU %d: active=%d lapic_id=%d\n", \
            $i, g_cpu_info[$i].active, g_cpu_info[$i].lapic_id
        set $i = $i + 1
    end
end

echo \n=== Starting GrahaOS Debug ===\n
echo Commands: check_tasks, check_cpus\n
continue