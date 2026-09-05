# Diagnostics used by the VS Code F5 Linux launch configurations.
# Each genuine debugger stop overwrites a workspace-local report that Codex can
# inspect without needing access to VS Code's Debug Console session.
set pagination off
set confirm off
set debuginfod enabled off
handle SIGPIPE nostop noprint pass
set logging file .gdb/gallery-crash.log
set logging overwrite on
set logging redirect on

define hook-stop
    set logging enabled on
    echo ==== MotionCam Fuse GDB stop ====\n
    info program
    echo \n==== Selected thread ====\n
    thread
    bt full 40
    echo \n==== Registers ====\n
    info registers
    echo \n==== All threads (bounded stacks) ====\n
    thread apply all bt 16
    set logging enabled off
end
