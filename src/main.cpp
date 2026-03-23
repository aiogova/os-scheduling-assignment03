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
            // FCFS
            if (shared_data->algorithm == ScheduleAlgorithm::FCFS) {
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
                // FCFS: just push at the end
                if (shared_data->algorithm == ScheduleAlgorithm::FCFS) { 
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
            }   

            //   - *Check if any processes have finished their I/O burst, and if so put that process back in the ready queue
            if (currentProcess->getState() == Process::State::IO) {
                currentProcess->updateProcess(current_time);

                if (currentProcess->getState() == Process::State::Ready) {
                    // FCFS: just push at the end
                    if (shared_data->algorithm == ScheduleAlgorithm::FCFS) { 
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
                }
            }

            //   - *Check if any running process need to be interrupted (RR time slice expires or newly ready process has higher priority)
            //     - NOTE: ensure processes are inserted into the ready queue at the proper position based on algorithm

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

    double first_half_end = finish_times[half - 1]; // time when first half finished
    double last_finish = finish_times[n - 1]; // time when all processes finished

    double throughput_first = half / first_half_end;
    double throughput_second = (n - half) / (last_finish - first_half_end);
    double throughput_total = n / last_finish;

    // calculate turnaround time
    double total_turnaround = 0.0;

    for (Process* p : processes) {
        total_turnaround += p->getTurnaroundTime();
    }

    double avg_turnaround = total_turnaround / processes.size();

    // print final statistics (use `printw()` for each print, and `refresh()` after all prints)
    printw("\nSimulation Complete!\n\n");
    //  - CPU utilization
    printw("\nCPU Utilization: %.2f%%\n", cpu_utilization * 100);
    //  - Throughput
    printw("\nThroughput:\n");
    //     - Average for first 50% of processes finished
    printw(" First 50%%: %.2f processes/sec\n", throughput_first);
    //     - Average for second 50% of processes finished
    printw(" Second 50%%: %.2f processes/sec\n", throughput_second);
    //     - Overall average
    printw(" Overall: %.2f processes/sec\n", throughput_total);
    //  - Average turnaround time
    printw("\nAverage Turnaround Time: %.2f seconds\n", avg_turnaround);
    //  - Average waiting time

    refresh();
    printw("\n\nPress any key to exit...");
    getch();

    // Clean up before quitting program
    processes.clear();
    endwin();

    // clean up heap memory
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
            while (true) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));

                currentProcess->updateProcess(currentTime());

                state = currentProcess->getState();

                //      - CPU burst time has elapsed (end the loop)
                if (state == Process::State::IO || state == Process::State::Terminated) {
                    break;
                }

                //      - Interrupted (RR time slice has elapsed or process preempted by higher priority process)
                //   - Place the process back in the appropriate queue
            }
            
            //      - I/O queue if CPU burst finished (and process not finished) -- no actual queue, simply set state to IO (done in updateProcess in process.cpp)
            //      - Terminated if CPU burst finished and no more bursts remain -- set state to Terminated (done in updateProcess in process.cpp)
            //      - *Ready queue if interrupted (be sure to modify the CPU burst time to now reflect the remaining time)
            
            //   - Wait context switching save time
            std::this_thread::sleep_for(std::chrono::milliseconds(shared_data->context_switch));
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
