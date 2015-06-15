/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Microsoft Corporation
 * 
 * -=- Robust Distributed System Nucleus (rDSN) -=- 
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#pragma once

# include <dsn/tool_api.h>
# include <dsn/internal/synchronize.h>
# if defined(__linux__)
# include <queue>
# include <stdio.h>		/* for perror() */
# include <unistd.h>		/* for syscall() */
# include <sys/syscall.h>	/* for __NR_* definitions */
# include <libaio.h>
# include <fcntl.h>		/* O_RDWR */
# include <string.h>		/* memset() */
# include <inttypes.h>	/* uint64_t */

namespace dsn {
    namespace tools {

		#define offsetof2(type, member) (size_t)&(((type*)0)->member)

		#define container_of(ptr, type, member) ({                      \
				        const decltype( ((type *)0)->member ) *__mptr = (ptr);    \
				        (type *)( (char *)__mptr - offsetof2(type,member) );})

        class native_linux_aio_provider : public aio_provider
        {
        public:
            native_linux_aio_provider(disk_engine* disk, aio_provider* inner_provider);
            ~native_linux_aio_provider();

            virtual handle_t open(const char* file_name, int flag, int pmode);
            virtual error_code close(handle_t hFile);
            virtual void    aio(aio_task_ptr& aio);            
            virtual disk_aio_ptr prepare_aio_context(aio_task* tsk);
			
			struct linux_disk_aio_context : public disk_aio
			{
				struct iocb cb;
				aio_task* tsk;
				native_linux_aio_provider* this_;
				utils::notify_event* evt;
				error_code err;
				uint32_t bytes;
			};

        protected:
            
			error_code aio_internal(aio_task_ptr& aio, bool async, __out_param uint32_t* pbytes = nullptr);

			static void aio_complete(io_context_t ctx, struct iocb *iocb, long res1, long res2);

			void get_event();

        private:
			::dsn::service::zlock _lock;
			std::queue<io_context_t> _ctx_q;
			std::queue<linux_disk_aio_context *> _aio_q;
        };
    }
}
# endif
