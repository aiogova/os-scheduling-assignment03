#include <iostream>
#include <string>
#include <list>
#include <vector>
#include <chrono>
#include <thread>
#include <mutex>
#include <ncurses.h>
#include "configreader.h"
#include "process.h"

// Shared data for all cores
typedef struct SchedulerData {
    std::mutex queue_mutex;
    ScheduleAlgorithm algorithm;
    uint32_t context_switch;
    uint32_t time_slice;
    std::list<Process*> ready_queue;
    bool all_terminated;
} SchedulerData;

void coreRunProcesses(uint8_t core_id, SchedulerData *data);
void printProcessOutput(std::vector<Process*>& processes);
std::string makeProgressString(double percent, uint32_t width);
uint64_t currentTime();
std::string processStateToString(Process::State state);

int main(int argc, char *argv[])
{
    // Ensure user entered a command line parameter for configuration file name
    if (argc < 2)
    {
        std::cerr << "Error: must specify configuration file" << std::endl;
        exit(EXIT_FAILURE);
    }

    // Declare variables used throughout main
    int i;
    SchedulerData *shared_data = new SchedulerData();
    std::vector<Process*> processes;

    // Read configuration file for scheduling simulation
    SchedulerConfig *config = scr::readConfigFile(argv[1]);

    // Store number of cores in local variable for future access
    uint8_t num_cores = config->cores;

    // Store configuration parameters in shared data object
    shared_data->algorithm = config->algorithm;
    shared_data->context_switch = config->context_switch;
    shared_data->time_slice = config->time_slice;
    shared_data->all_terminated = false;

    // Create processes
    uint64_t start = currentTime();
    for (i = 0; i < config->num_processes; i++)
    {
        Process *p = new Process(config->processes[i], start);
        processes.push_back(p);
        // If process should be launched immediately, add to ready queue
        if (p->getState() == Process::State::Ready)
        {
            // FCFS or RR: push to back
            if (shared_data->algorithm == ScheduleAlgorithm::FCFS ||
                shared_data->algorithm == ScheduleAlgorithm::RR) {
                shared_data->ready_queue.push_back(p);
            }
            // SJF
            else if (shared_data->algorithm == ScheduleAlgorithm::SJF) {

                auto it = shared_data->ready_queue.begin();

                while (it != shared_data->ready_queue.end()) {
                    if ((*it)->getRemainingTime() > p->getRemainingTime()) {
                        break;
                    }
                    ++it;
                }

                shared_data->ready_queue.insert(it, p);
            }
            // PP: priority-sorted insertion (lower number = higher priority, FCFS for ties)
            else if (shared_data->algorithm == ScheduleAlgorithm::PP) {
                auto it = shared_data->ready_queue.begin();
                while (it != shared_data->ready_queue.end()) {
                    if ((*it)->getPriority() > p->getPriority()) {
                        break;
                    }
                    ++it;
                }
                shared_data->ready_queue.insert(it, p);
            }
        }
    }

    // Free configuration data from memory
    scr::deleteConfig(config);

    // Launch 1 scheduling thread per cpu core
    std::thread *schedule_threads = new std::thread[num_cores];
    for (i = 0; i < num_cores; i++)
    {
        schedule_threads[i] = std::thread(coreRunProcesses, i, shared_data);
    }

    // Main thread work goes here
    initscr();
    while (!(shared_data->all_terminated))
    {
        // Do the following:
        //   - Get current time
        uint64_t current_time = currentTime();
        shared_data->queue_mutex.lock();

        //   - *Check if any processes need to move from NotStarted to Ready (based on elapsed time), and if so put that process in the ready queue
        for (Process* currentProcess : processes) {
            if (currentProcess->getState() == Process::State::NotStarted && currentProcess->getStartTime() <= (current_time - start)) {
                currentProcess->setState(Process::State::Ready, current_time);
                // FCFS or RR: push to back
                if (shared_data->algorithm == ScheduleAlgorithm::FCFS ||
                    shared_data->algorithm == ScheduleAlgorithm::RR) { 
                    shared_data->ready_queue.push_back(currentProcess); 
                }
                // SJF
                else if (shared_data->algorithm == ScheduleAlgorithm::SJF) {

                    auto it = shared_data->ready_queue.begin();

                    while (it != shared_data->ready_queue.end()) {
                        if ((*it)->getRemainingTime() > currentProcess->getRemainingTime()) {
                            break;
                        }
                        ++it;
                    }

                    shared_data->ready_queue.insert(it, currentProcess);
                }
                // PP: priority-sorted insertion + preemption check
                else if (shared_data->algorithm == ScheduleAlgorithm::PP) {
                    auto it = shared_data->ready_queue.begin();
                    while (it != shared_data->ready_queue.end()) {
                        if ((*it)->getPriority() > currentProcess->getPriority()) {
                            break;
                        }
                        ++it;
                    }
                    shared_data->ready_queue.insert(it, currentProcess);
                    // Preempt the lowest-priority running process if it has lower priority
                    Process* to_preempt = nullptr;
                    for (Process* p : processes) {
                        if (p->getState() == Process::State::Running) {
                            if (to_preempt == nullptr || p->getPriority() > to_preempt->getPriority()) {
                                to_preempt = p;
                            }
                        }
                    }
                    if (to_preempt != nullptr && to_preempt->getPriority() > currentProcess->getPriority()) {
                        to_preempt->interrupt();
                    }
                }
            }   

            //   - *Check if any processes have finished their I/O burst, and if so put that process back in the ready queue
            if (currentProcess->getState() == Process::State::IO) {
                currentProcess->updateProcess(current_time);

                if (currentProcess->getState() == Process::State::Ready) {
                    // FCFS or RR: push to back
                    if (shared_data->algorithm == ScheduleAlgorithm::FCFS ||
                        shared_data->algorithm == ScheduleAlgorithm::RR) { 
                        shared_data->ready_queue.push_back(currentProcess);
                    }
                    // SJF
                    else if (shared_data->algorithm == ScheduleAlgorithm::SJF) {

                        auto it = shared_data->ready_queue.begin();

                        while (it != shared_data->ready_queue.end()) {
                            if ((*it)->getRemainingTime() > currentProcess->getRemainingTime()) {
                                break;
                            }
                            ++it;
                        }

                        shared_data->ready_queue.insert(it, currentProcess);
                    }
                    // PP: priority-sorted insertion + preemption check
                    else if (shared_data->algorithm == ScheduleAlgorithm::PP) {
                        auto it = shared_data->ready_queue.begin();
                        while (it != shared_data->ready_queue.end()) {
                            if ((*it)->getPriority() > currentProcess->getPriority()) {
                                break;
                            }
                            ++it;
                        }
                        shared_data->ready_queue.insert(it, currentProcess);
                        // Preempt the lowest-priority running process if it has lower priority
                        Process* to_preempt = nullptr;
                        for (Process* p : processes) {
                            if (p->getState() == Process::State::Running) {
                                if (to_preempt == nullptr || p->getPriority() > to_preempt->getPriority()) {
                                    to_preempt = p;
                                }
                            }
                        }
                        if (to_preempt != nullptr && to_preempt->getPriority() > currentProcess->getPriority()) {
                            to_preempt->interrupt();
                        }
                    }
                }
            }

            //   - *Check if any running process need to be interrupted (RR time slice expires or newly ready process has higher priority)
            //     - NOTE: ensure processes are inserted into the ready queue at the proper position based on algorithm

            // Accumulate wait time for processes sitting in the ready queue
            if (currentProcess->getState() == Process::State::Ready) {
                currentProcess->updateProcess(current_time);
            }

        }
            
        //   - * = accesses shared data (ready queue), so be sure to use proper synchronization
        shared_data->queue_mutex.unlock();

        //   - Determine if all processes are in the terminated state
        shared_data->all_terminated = true;
        for (Process* currentProcess : processes) {
            if (currentProcess->getState() != Process::State::Terminated) {
                shared_data->all_terminated = false;
                break;
            }
        }

        // Maybe simply print progress bar for all procs?
        printProcessOutput(processes);

        // sleep 50 ms
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // clear outout
        if (!shared_data->all_terminated) {
            erase();
        }
        
    }


    // wait for threads to finish
    for (i = 0; i < num_cores; i++)
    {
        schedule_threads[i].join();
    }

    uint64_t end = currentTime();

    // calculate cpu utilization
    double total_cpu_time = 0.0;

    for (Process* currentProcess : processes) {
        total_cpu_time += currentProcess->getCpuTime(); // in seconds
    }

    double total_simulation_time = (end - start) / 1000.0; // ms to seconds

    double cpu_utilization = total_cpu_time / (total_simulation_time * num_cores);

    // calculate throughput
    std::vector<double> finish_times; // stores the time the processes finished

    for (Process* p : processes) {
        finish_times.push_back((p->getFinishTime() - start) / 1000.0); // how much time after the simulation started did the process finish
    }

    std::sort(finish_times.begin(), finish_times.end()); // sort the list so the processes with the quickest finish times are first

    int n = finish_times.size();
    int half = n / 2;

    // edge case (don't want index of -1 later)
    if (half == 0) {
        half = 1;
    }

    double first_half_end = finish_times[half - 1]; // time when first half finished
    double last_finish = finish_times[n - 1]; // time when all processes finished

    double throughput_first = half / first_half_end;
    double throughput_second;
    double second_interval = last_finish - first_half_end;
    // fix division by zero bug
    if (second_interval == 0) {
        throughput_second = 0.0;
    } else {
        throughput_second = (n - half) / second_interval;
    }
    double throughput_total = n / last_finish;

    // calculate turnaround time
    double total_turnaround = 0.0;

    for (Process* p : processes) {
        total_turnaround += p->getTurnaroundTime();
    }

    double avg_turnaround = total_turnaround / processes.size();

    // calculate waiting time
    double total_wait_time = 0.0;
    for (Process* p : processes) {
        total_wait_time += p->getWaitTime();
    }
    double avg_wait_time = total_wait_time / processes.size();
   
    endwin();

    // print final statistics 
    std::cout << "\nSimulation Complete!\n\n";
    //  - CPU utilization
    std::cout << "CPU Utilization: " << cpu_utilization * 100 << "%\n\n";
    //  - Throughput
    std::cout << "Throughput:\n";
    //     - Average for first 50% of processes finished
    std::cout << " First 50%: " << throughput_first << " processes/sec\n";
    //     - Average for second 50% of processes finished
    std::cout << " Second 50%: " << throughput_second << " processes/sec\n";
    //     - Overall average
    std::cout << " Overall: " << throughput_total << " processes/sec\n\n";
    //  - Average turnaround time
    std::cout << "Average Turnaround Time: " << avg_turnaround << " seconds\n\n";
    //  - Average waiting time
    std::cout << "Average Wait Time: " << avg_wait_time << " seconds\n\n";


    // Clean up before quitting program
    for (Process* p : processes) {
        delete p;
    }

    processes.clear();
    
    delete[] schedule_threads;

    delete shared_data;

    return 0;
}

void coreRunProcesses(uint8_t core_id, SchedulerData *shared_data)
{
    // Work to be done by each core idependent of the other cores
    // Repeat until all processes in terminated state:
    while ((shared_data->all_terminated) == false) {    
        //   - *Get process at front of ready queue
        Process *currentProcess = nullptr;
        shared_data->queue_mutex.lock();
        // if the queue is not empty
        if (!(shared_data->ready_queue.empty())) {
            // Grab the process at the front of the ready queue
            currentProcess = shared_data->ready_queue.front();
            // Remove the process at the front of the ready queue
            shared_data->ready_queue.pop_front();
        }
        shared_data->queue_mutex.unlock();

        //   - IF READY QUEUE WAS NOT EMPTY
        if (currentProcess != nullptr) { // if it is nullptr, that means the queue is empty, and we don't just want to check ready_queue.empty again because we unlocked the mutex and another thread could have already changed the queue
            //    - Wait context switching load time
            std::this_thread::sleep_for(std::chrono::milliseconds(shared_data->context_switch));
            //    - Simulate the processes running (i.e. sleep for short bits, e.g. 5 ms, and call the processes `updateProcess()` method)
            //      until one of the following:
            currentProcess->setState(Process::State::Running, currentTime());
            currentProcess->setCpuCore(core_id);
            currentProcess->setBurstStartTime(currentTime());

            Process::State state;
            uint64_t slice_start = currentTime();
            while (true) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));

                currentProcess->updateProcess(currentTime());

                state = currentProcess->getState();

                //      - CPU burst time has elapsed (end the loop)
                if (state == Process::State::IO || state == Process::State::Terminated) {
                    break;
                }

                //      - RR: time slice has elapsed
                if (shared_data->algorithm == ScheduleAlgorithm::RR &&
                    (currentTime() - slice_start) >= shared_data->time_slice) {
                    break;
                }

                //      - PP: preempted by higher-priority process (signaled by main thread)
                if (shared_data->algorithm == ScheduleAlgorithm::PP &&
                    currentProcess->isInterrupted()) {
                    currentProcess->interruptHandled();
                    break;
                }
            }

            //   - Wait context switching save time
            std::this_thread::sleep_for(std::chrono::milliseconds(shared_data->context_switch));

            //      - *Ready queue if interrupted (burst time already reflects remaining time via updateProcess)
            if (state == Process::State::Running) {
                currentProcess->setState(Process::State::Ready, currentTime());
                currentProcess->setCpuCore(-1);
                shared_data->queue_mutex.lock();
                if (shared_data->algorithm == ScheduleAlgorithm::RR) {
                    shared_data->ready_queue.push_back(currentProcess);
                }
                else if (shared_data->algorithm == ScheduleAlgorithm::PP) {
                    auto it = shared_data->ready_queue.begin();
                    while (it != shared_data->ready_queue.end()) {
                        if ((*it)->getPriority() > currentProcess->getPriority()) {
                            break;
                        }
                        ++it;
                    }
                    shared_data->ready_queue.insert(it, currentProcess);
                }
                shared_data->queue_mutex.unlock();
            }
        }
        
        //  - IF READY QUEUE WAS EMPTY
        else {
            //   - Wait short bit (i.e. sleep 5 ms)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        //  - * = accesses shared data (ready queue), so be sure to use proper synchronization
    }
}

void printProcessOutput(std::vector<Process*>& processes)
{
    printw("|   PID | Priority |    State    | Core |               Progress               |\n"); // 36 chars for prog
    printw("+-------+----------+-------------+------+--------------------------------------+\n");
    for (int i = 0; i < processes.size(); i++)
    {
        if (processes[i]->getState() != Process::State::NotStarted)
        {
            uint16_t pid = processes[i]->getPid();
            uint8_t priority = processes[i]->getPriority();
            std::string process_state = processStateToString(processes[i]->getState());
            int8_t core = processes[i]->getCpuCore();
            std::string cpu_core = (core >= 0) ? std::to_string(core) : "--";
            double total_time = processes[i]->getTotalRunTime();
            double completed_time = total_time - processes[i]->getRemainingTime();
            std::string progress = makeProgressString(completed_time / total_time, 36);
            printw("| %5u | %8u | %11s | %4s | %36s |\n", pid, priority,
                   process_state.c_str(), cpu_core.c_str(), progress.c_str());
        }
    }
    refresh();
}

std::string makeProgressString(double percent, uint32_t width)
{
    uint32_t n_chars = percent * width;
    std::string progress_bar(n_chars, '#');
    progress_bar.resize(width, ' ');
    return progress_bar;
}

uint64_t currentTime()
{
    uint64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
    return ms;
}

std::string processStateToString(Process::State state)
{
    std::string str;
    switch (state)
    {
        case Process::State::NotStarted:
            str = "not started";
            break;
        case Process::State::Ready:
            str = "ready";
            break;
        case Process::State::Running:
            str = "running";
            break;
        case Process::State::IO:
            str = "i/o";
            break;
        case Process::State::Terminated:
            str = "terminated";
            break;
        default:
            str = "unknown";
            break;
    }
    return str;
}
