all: cgroup

cgroup: cgroup.cpp
	g++ $< -o $@ -std=c++17 -lpthread -Wall -g
