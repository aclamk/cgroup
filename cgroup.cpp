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

uint64_t notification_limit;
std::list<std::pair<void*,size_t>> memory;
std::mutex memory_lock;

void allocating()
{
  while(true) {
    usleep(10*1000);
    std::cout << "Before: " << std::fixed << std::setprecision(6) << tnow() << std::endl;
    uint64_t size = ((rand() % (notification_limit / 50) ) & ~0xfff) + 0x1000 ;
    //std::cout << size << std::endl;
    void* ptr = mmap(nullptr, size, PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    std::cout << ptr << ":" << size << std::endl;
    
    memset((char*)ptr, 0, size);
    memory_lock.lock();
    memory.push_back({ptr, size});
    memory_lock.unlock();
    std::cout << "After:  " << tnow() << std::endl;    
  }
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

private:
  int event_fd;
  int m_usage_fd;
  bool is_high;
  
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
};

int main(int argc, char** argv)
{
  char data[100];
  int w;
  auto x = cgroup_memory_get();
  if (!x.first) {
    std::cerr << "No cgroups" << std::endl;
  }
  std::string cgroup_memory = x.second;
  std::cout << "In cgroup memory: " << x.second << std::endl;

  if (argc > 1) {
    notification_limit = strtoll(argv[1], nullptr, 0);
  } else {
    int fd = open(("/sys/fs/cgroup/memory/"+cgroup_memory+"/memory.limit_in_bytes").c_str(), O_RDONLY);
    if (fd < 0) return -1;
    w = read(fd, data, 99);
    data[w] = 0;
    close (fd);
    notification_limit = strtoll(data, nullptr, 0) / 100 * 95; //95%
  }
  std::cout << "Notification level: " << notification_limit << std::endl;
  
  int event_fd = eventfd(0, 0);
  int m_usage_fd = open(("/sys/fs/cgroup/memory/"+cgroup_memory+"/memory.usage_in_bytes").c_str(), O_RDONLY);
  int control_fd = open(("/sys/fs/cgroup/memory/"+cgroup_memory+"/cgroup.event_control").c_str(), O_WRONLY);
  if (event_fd < 0 || m_usage_fd < 0 || control_fd < 0)
    return -1;
  w = sprintf(data, "%d %d %ld", event_fd, m_usage_fd, notification_limit);
  if (write(control_fd, data, w) <= 0)
    return -1;
  close(control_fd);

  std::thread t1(allocating);
  
  while (true) {
    uint64_t current_level;
    eventfd_t event;
    if (read(event_fd, &event, 8) != 8)
      break;
    std::cout << "event=" << event << std::endl;
    //fcntl(event_fd, F_SETFD, (fcntl(event_fd, F_GETFD, 0)|O_NONBLOCK));
    
    std::cout << "Current level: " << get_usage(m_usage_fd) << " now=" << tnow() << std::endl;
    //if (current_level > notification_limit)
    do {
      //if (current_level > notification_limit)
      {
	assert(memory.size()>=0);
	auto ptr = memory.front();
	std::cout << "Free " << ptr.first << std::endl;
	memory.pop_front();
	munmap(ptr.first, ptr.second);
      }
      std::cout << __LINE__ << std::endl;
      struct pollfd poll_fd;
      poll_fd.fd = event_fd;
      poll_fd.events = POLLIN;
      if (poll(&poll_fd, 1, 0) > 0) {
	std::cout << "signalled" << std::endl;
	 eventfd_t event;
	 //read(event_fd, &event, 8);
	 usleep(200*1000);
	break;
      }
      std::cout << __LINE__ << std::endl;
    } while (true);//(current_level > notification_limit);
    //fcntl(event_fd, F_SETFD, (fcntl(event_fd, F_GETFD)&~O_NONBLOCK));
  }
  return -1;
}
