#include "process.h"

// Process class methods
Process::Process(ProcessDetails details, uint64_t current_time)
{
    int i;
    pid = details.pid;
    start_time = details.start_time;
    num_bursts = details.num_bursts;
    current_burst = 0;
    burst_times = new uint32_t[num_bursts];
    for (i = 0; i < num_bursts; i++)
    {
        burst_times[i] = details.burst_times[i];
    }
    priority = details.priority;
    state = (start_time == 0) ? State::Ready : State::NotStarted;
    if (state == State::Ready)
    {
        launch_time = current_time;
    }
    is_interrupted = false;
    core = -1;
    turn_time = 0;
    wait_time = 0;
    cpu_time = 0;
    total_time = 0;
    for (i = 0; i < num_bursts; i+=2)
    {
        total_time += burst_times[i];
    }
    remain_time = total_time;
}

Process::~Process()
{
    delete[] burst_times;
}

uint16_t Process::getPid() const
{
    return pid;
}

uint32_t Process::getStartTime() const
{
    return start_time;
}

uint8_t Process::getPriority() const
{
    return priority;
}

uint64_t Process::getBurstStartTime() const
{
    return burst_start_time;
}

Process::State Process::getState() const
{
    return state;
}

bool Process::isInterrupted() const
{
    return is_interrupted;
}

int8_t Process::getCpuCore() const
{
    return core;
}

double Process::getTurnaroundTime() const
{
    return (double)turn_time / 1000.0;
}

double Process::getWaitTime() const
{
    return (double)wait_time / 1000.0;
}

double Process::getCpuTime() const
{
    return (double)cpu_time / 1000.0;
}

double Process::getTotalRunTime() const
{
    return (double)total_time / 1000.0;
}

double Process::getRemainingTime() const
{
    return (double)remain_time / 1000.0;
}

void Process::setBurstStartTime(uint64_t current_time)
{
    burst_start_time = current_time;
}

void Process::setState(State new_state, uint64_t current_time)
{
    if (state == State::NotStarted && new_state == State::Ready)
    {
        launch_time = current_time;
    }
    state = new_state;
}

void Process::setCpuCore(int8_t core_num)
{
    core = core_num;
}

void Process::interrupt()
{
    is_interrupted = true;
}

void Process::interruptHandled()
{
    is_interrupted = false;
}

void Process::updateProcess(uint64_t current_time)
{
    // use `current_time` to update turnaround time, wait time, burst times, 
    // cpu time, and remaining time

    uint64_t elapsed = current_time - burst_start_time;

    // prevent burst_times from ever becoming "negative"
    if (elapsed >= burst_times[current_burst]) {
        elapsed = burst_times[current_burst];
    }

    burst_start_time = current_time;

    // update turnaround time (total time since launch)
    turn_time = current_time - launch_time;

    if (state == State::Running)
    {
        // update cpu time
        cpu_time += elapsed;

        // update burst times
        burst_times[current_burst] -= elapsed;

        // update remaining time
        remain_time -= elapsed;

        // if the current burst finished
        if (burst_times[current_burst] <= 0)
        {
            // move on to the next burst
            current_burst++;

            // if more bursts remain, the process moves to IO
            if (current_burst < num_bursts)
            {
                state = State::IO;
                burst_start_time = current_time;
                core = -1;
            }
            // else, no more bursts remain, which means the process has terminated
            else
            {
                state = State::Terminated;
                core = -1;
            }
        }
    }
    // if the process is currently in an IO burst
    else if (state == State::IO)
    {
        // update the burst times
        burst_times[current_burst] -= elapsed;

        // if the IO burst has finished
        if (burst_times[current_burst] <= 0)
        {
            // move on to the next burst
            current_burst++;

            // set the state to Ready (ready to be put back on the CPU)
            state = State::Ready;
        }
    }
    // if waiting in the ready queue
    else if (state == State::Ready)
    {
        wait_time += elapsed;
    }
}

void Process::updateBurstTime(int burst_idx, uint32_t new_time)
{
    burst_times[burst_idx] = new_time;
}
