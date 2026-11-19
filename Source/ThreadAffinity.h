#pragma once

#if defined(__linux__) || defined(__ANDROID__)
#include <cstdio>
#include <cstring>
#include <sched.h>
#include <unistd.h>
#include <vector>
#endif

namespace ThreadAffinity
{
	enum class Role
	{
		EePrime,   //Main PS2VM EE thread: pin to highest-clock core(s) on heterogeneous SoCs
		BigWorker, //Generic big-core worker
		Vu1Worker, //VU1/GIF worker: lower half of non-prime big cores so it never preempts GS Thread
		GsWorker,  //GS Vulkan thread: upper half of non-prime big cores so it never preempts VU1/GIF Thread
	};

	inline void ApplyCurrentThread(Role role)
	{
#if defined(__linux__) || defined(__ANDROID__)
		struct Topology
		{
			bool heterogeneous = false;
			cpu_set_t primeSet;
			cpu_set_t bigWorkerSet;
			cpu_set_t vu1WorkerSet;
			cpu_set_t gsWorkerSet;
		};
		static const Topology topo = []() {
			Topology t;
			CPU_ZERO(&t.primeSet);
			CPU_ZERO(&t.bigWorkerSet);
			CPU_ZERO(&t.vu1WorkerSet);
			CPU_ZERO(&t.gsWorkerSet);

			long ncpu = sysconf(_SC_NPROCESSORS_CONF);
			if(ncpu <= 1) return t;
			if(ncpu > CPU_SETSIZE) ncpu = CPU_SETSIZE;

			std::vector<long> maxFreq(static_cast<size_t>(ncpu), 0);
			long globalMax = 0;
			long globalMin = 0;
			int validCount = 0;
			for(long c = 0; c < ncpu; c++)
			{
				char path[96];
				snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%ld/cpufreq/cpuinfo_max_freq", c);
				FILE* f = fopen(path, "r");
				if(!f) continue;
				long freq = 0;
				if(fscanf(f, "%ld", &freq) == 1 && freq > 0)
				{
					maxFreq[static_cast<size_t>(c)] = freq;
					if(validCount == 0 || freq > globalMax) globalMax = freq;
					if(validCount == 0 || freq < globalMin) globalMin = freq;
					validCount++;
				}
				fclose(f);
			}
			if(validCount < 2 || globalMax <= globalMin) return t;

			int primeCount = 0;
			std::vector<int> nonPrimeBigCores;
			for(long c = 0; c < ncpu; c++)
			{
				long freq = maxFreq[static_cast<size_t>(c)];
				if(freq == globalMax)
				{
					CPU_SET(static_cast<int>(c), &t.primeSet);
					primeCount++;
				}
				else if(freq > globalMin)
				{
					CPU_SET(static_cast<int>(c), &t.bigWorkerSet);
					nonPrimeBigCores.push_back(static_cast<int>(c));
				}
			}
			if(primeCount == 0) return t;

			//On 3-tier SoCs with >=2 non-prime big cores (e.g. 1 Prime + 4 Big + 3 Little),
			//split VU1/GIF and GS threads onto disjoint big-core halves so they never preempt each other.
			if(nonPrimeBigCores.size() >= 2)
			{
				size_t mid = nonPrimeBigCores.size() / 2;
				for(size_t i = 0; i < mid; i++)
				{
					CPU_SET(nonPrimeBigCores[i], &t.vu1WorkerSet);
				}
				for(size_t i = mid; i < nonPrimeBigCores.size(); i++)
				{
					CPU_SET(nonPrimeBigCores[i], &t.gsWorkerSet);
				}
			}
			else
			{
				//On 2-tier SoCs (e.g. 4 big + 4 little), allow workers to use all non-little cores.
				CPU_ZERO(&t.bigWorkerSet);
				for(long c = 0; c < ncpu; c++)
				{
					if(maxFreq[static_cast<size_t>(c)] > globalMin)
					{
						CPU_SET(static_cast<int>(c), &t.bigWorkerSet);
					}
				}
				t.vu1WorkerSet = t.bigWorkerSet;
				t.gsWorkerSet = t.bigWorkerSet;
			}
			t.heterogeneous = true;
			return t;
		}();

		if(!topo.heterogeneous) return;
		const cpu_set_t& mask = (role == Role::EePrime)     ? topo.primeSet
		                        : (role == Role::Vu1Worker) ? topo.vu1WorkerSet
		                        : (role == Role::GsWorker)  ? topo.gsWorkerSet
		                                                    : topo.bigWorkerSet;
		sched_setaffinity(0, sizeof(cpu_set_t), &mask);
#else
		(void)role;
#endif
	}
}
