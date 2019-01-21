/*
Copyright (c) 2009-2018 Roger Light <roger@atchoo.org>

All rights reserved. This program and the accompanying materials
are made available under the terms of the Eclipse Public License v1.0
and Eclipse Distribution License v1.0 which accompany this distribution.
 
The Eclipse Public License is available at
   http://www.eclipse.org/legal/epl-v10.html
and the Eclipse Distribution License is available at
  http://www.eclipse.org/org/documents/edl-v10.php.
 
Contributors:
   Roger Light - initial implementation and documentation.

   对内存操作的封装文件，实现了对常用的内存申请与释放相关的系统调用函数
   如果文件中定义了宏REAL_WITH_MEMORY_TRACKING，则这些封装函数只是对系统函数进行封装，不做任何额外操作
   如果定义了REAL_WITH_MEMORY_TRACKING宏，则会在内存申请和释放时分别记录所申请或释放内存的大小
*/
#include "config.h"

#include <stdlib.h>
#include <string.h>

#include "memory_mosq.h"
// #include "mosquitto_broker_internal.h"

#ifdef REAL_WITH_MEMORY_TRACKING
#  if defined(__APPLE__)
#    include <malloc/malloc.h>
#    define malloc_usable_size malloc_size
#  elif defined(__FreeBSD__)
#    include <malloc_np.h>
#  else
#    include <malloc.h>
#  endif
#endif

#ifdef REAL_WITH_MEMORY_TRACKING
static unsigned long memcount = 0;
static unsigned long max_memcount = 0;
#endif

#ifdef WITH_BROKER
static size_t mem_limit = 0;
void memory__set_limit(size_t lim)
{
#ifdef LINUX
	struct rlimit r;

	r.rlim_cur = lim;
	r.rlim_max = lim;

	setrlimit(RLIMIT_CPU, &r);

	mem_limit = 0;
#else
	mem_limit = lim;
#endif
}
#endif
//对系统函数calloc的封装
void *mosquitto__calloc(size_t nmemb, size_t size)
{
#ifdef REAL_WITH_MEMORY_TRACKING
	if(mem_limit && memcount + size > mem_limit){
		return NULL;
	}
#endif
	void *mem = calloc(nmemb, size);

#ifdef REAL_WITH_MEMORY_TRACKING
	if(mem){
		memcount += malloc_usable_size(mem);
		if(memcount > max_memcount){
			max_memcount = memcount;
		}
	}
#endif

	return mem;
}
//对系统函数free的封装
void mosquitto__free(void *mem)
{
#ifdef REAL_WITH_MEMORY_TRACKING
	if(!mem){
		return;
	}
	memcount -= malloc_usable_size(mem);
	// printf("free %d bytes\n",malloc_usable_size(mem));
#endif
	free(mem);
}
//对系统函数malloc的封装
void *mosquitto__malloc(size_t size)
{
#ifdef REAL_WITH_MEMORY_TRACKING
	if(mem_limit && memcount + size > mem_limit){
		return NULL;
	}
#endif
	void *mem = malloc(size);

#ifdef REAL_WITH_MEMORY_TRACKING
	if(mem){
		memcount += malloc_usable_size(mem);
		if(memcount > max_memcount){
			max_memcount = memcount;
		}
		// printf("malloc %d bytes\n",malloc_usable_size(mem));
	}
#endif

	return mem;
}

#ifdef REAL_WITH_MEMORY_TRACKING
unsigned long mosquitto__memory_used(void)
{
	return memcount;
}

unsigned long mosquitto__max_memory_used(void)
{
	return max_memcount;
}
#endif
//对系统函数realloc的封装
void *mosquitto__realloc(void *ptr, size_t size)
{
#ifdef REAL_WITH_MEMORY_TRACKING
	if(mem_limit && memcount + size > mem_limit){
		return NULL;
	}
#endif
	void *mem;
#ifdef REAL_WITH_MEMORY_TRACKING
	if(ptr){
		memcount -= malloc_usable_size(ptr);
	}
#endif
	mem = realloc(ptr, size);

#ifdef REAL_WITH_MEMORY_TRACKING
	if(mem){
		memcount += malloc_usable_size(mem);
		if(memcount > max_memcount){
			max_memcount = memcount;
		}
	}
#endif

	return mem;
}
//对系统函数strdup的封装
char *mosquitto__strdup(const char *s)
{
#ifdef REAL_WITH_MEMORY_TRACKING
	if(mem_limit && memcount + strlen(s) > mem_limit){
		return NULL;
	}
#endif
	char *str = strdup(s);

#ifdef REAL_WITH_MEMORY_TRACKING
	if(str){
		memcount += malloc_usable_size(str);
		if(memcount > max_memcount){
			max_memcount = memcount;
		}
		// printf("strdup %d bytes\n",malloc_usable_size(str));
	}
#endif

	return str;
}

