#include <sys/eventfd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <tuple>
#include <sstream>
#include <iostream>
#include <fstream>
#include <sys/time.h>
#include <thread>
#include <iomanip>
#include <vector>
#include <list>
#include <string.h>
#include <mutex>
#include <assert.h>
#include <sys/mman.h>
#include <poll.h>
#include <atomic>

std::pair<bool, std::string> cgroup_memory_get()
{
  std::ifstream proc_cgroup("/proc/self/cgroup");
  if (proc_cgroup.good())
  {
    while (!proc_cgroup.eof())
    {
      std::string line;
      std::string section;
      std::string name;
      std::string value;
      if (std::getline(proc_cgroup,line))
      {
	std::stringstream is(line);
	if (!std::getline(is, section, ':')) continue;
	if (!std::getline(is, name, ':')) continue;
	if (!std::getline(is, value)) continue;
	if (name == "memory")
	  return {true, value};
      }
    }
  }
  return {false, ""};
}

double tnow()
{
  struct timeval n;
  gettimeofday(&n, NULL);
  return n.tv_sec + n.tv_usec/1000000.;
}  

uint64_t get_usage(int fd)
{
  char data[100];
  int w = pread(fd, data, 99, 0);
  if(w>=0) data[w]=0;
  return strtol(data, nullptr, 0);
}


class memory_monitor
{
public:
  memory_monitor(int event_fd, int m_usage_fd, uint64_t high_level) :
    event_fd(event_fd),
    m_usage_fd(m_usage_fd) {
    is_high = get_current_level() >= high_level; 
  }
  bool is_level_high() {
    return is_high;
  }
  uint64_t get_current_level() {
    char data[100];
    int w = pread(m_usage_fd, data, 99, 0);
    if (w>=0) data[w]=0;
    return strtol(data, nullptr, 0);
  }
  /**
     check, or wait for change in high level signalisation
     returns: true - level changed
              false - no change occured

     note: It is possible that memory use level has crossed high_level several times.
           It is necessary to read \ref is_level_high to query state.
  */
  bool wait_change(int timeout_ms) {
    struct pollfd poll_fd;
    poll_fd.fd = event_fd;
    poll_fd.events = POLLIN;
    int r;
    r = poll(&poll_fd, 1, timeout_ms);
    if (r>0) {
      eventfd_t event;
      if (read(event_fd, &event, 8) == 8) {
	is_high = is_high ^ (event & 1);
      }
    }
    return r>0;    
  }

  static memory_monitor* create(uint64_t high_level) {
    static auto cgroup_memory = cgroup_memory_get();
    if (!cgroup_memory.first)
      return nullptr;
    int event_fd = eventfd(0, 0);
    int m_usage_fd = open(("/sys/fs/cgroup/memory/"+
			   cgroup_memory.second+
			   "/memory.usage_in_bytes").c_str(), O_RDONLY);
    int control_fd = open(("/sys/fs/cgroup/memory/"+
			   cgroup_memory.second+
			   "/cgroup.event_control").c_str(), O_WRONLY);
    if (event_fd >= 0 && m_usage_fd >=0 && control_fd >= 0) {
      char data[100];
      int w = snprintf(data, 99, "%d %d %ld", event_fd, m_usage_fd, high_level);
      if (write(control_fd, data, w) > 0) {
	close(control_fd);
	return new memory_monitor(event_fd, m_usage_fd, high_level);
      }
    }
    if (event_fd >= 0)
      close (event_fd);
    if (m_usage_fd >= 0)
      close (m_usage_fd);
    if (control_fd >= 0)
      close (control_fd);
    return nullptr;
  }

private:
  int event_fd;
  int m_usage_fd;
  bool is_high;
};


memory_monitor* monitor;
uint64_t notification_limit;
std::list<std::pair<void*,size_t>> memory;
std::mutex memory_lock;
std::atomic<double> operation_time;
std::atomic<bool> allocating_stop;

void allocating()
{
  do {
    uint64_t size = notification_limit / 50; //2%
    if (size > 1024*1024)
      size = 1024*1024;
    size = ((rand() % size) & ~0xfff) + 0x1000 ;
    void* ptr = mmap(nullptr, size, PROT_READ|PROT_WRITE,
		     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    operation_time.store(tnow());
    memset((char*)ptr, 0, size);
    std::cout << "Mem=" << monitor->get_current_level() << std::endl;
    memory_lock.lock();
    memory.push_back({ptr, size});
    memory_lock.unlock();
    usleep(10*1000);
  } while (!allocating_stop.load());
}


int main(int argc, char** argv)
{
  auto x = cgroup_memory_get();
  if (!x.first) {
    std::cerr << "No cgroups" << std::endl;
    return -1;
  }
  std::string cgroup_memory = x.second;
  std::cout << "In cgroup memory: " << x.second << std::endl;

  if (argc > 1) {
    notification_limit = strtoll(argv[1], nullptr, 0);
  } else {
    int fd = open(("/sys/fs/cgroup/memory/"+cgroup_memory+"/memory.usage_in_bytes").c_str(), O_RDONLY);
    if (fd < 0) return -1;
    char data[100];
    int w  = read(fd, data, 99);
    data[w] = 0;
    close (fd);
    notification_limit = strtoll(data, nullptr, 0) + 10*1024*1024;
  }
  std::cout << "Notification level: " << notification_limit << std::endl;

  monitor = memory_monitor::create(notification_limit);
  std::thread t1(allocating);
  double latency_sum = 0;
  int latency_samples = 0;

  double start_time = tnow();
  while (tnow() - start_time < 10) {
    if (!monitor->is_level_high())
      if (!monitor->wait_change(-1))
	break;
    double latency = tnow() - operation_time.load();
    std::cout << "trigger latency=" << std::fixed << std::setprecision(6) << latency << std::endl;
    latency_sum += latency;
    latency_samples ++;
    while (monitor->is_level_high())
    {
      assert(memory.size()>=0);
      auto ptr = memory.front();
      memory.pop_front();
      munmap(ptr.first, ptr.second);
      monitor->wait_change(0);
    };
  }
  allocating_stop.store(true);
  t1.join();
  std::cout << "Average reaction latency: " <<
	    std::fixed << std::setprecision(6) << latency_sum / latency_samples << std::endl;
  return 0;
}
