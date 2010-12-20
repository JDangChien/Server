#include "../StdAfx.h"

#ifdef _MSC_VER
#pragma warning (disable : 4244)
#endif

#include "frame_consumer_device.h"

#include "../format/video_format.h"

#include <common/concurrency/executor.h>

#include <tbb/concurrent_queue.h>
#include <tbb/atomic.h>

#include <boost/date_time/posix_time/posix_time.hpp>

#include <boost/range/algorithm_ext/erase.hpp>
#include <boost/range/algorithm.hpp>

namespace caspar { namespace core {

class clock_sync
{
public:
	clock_sync() : time_(boost::posix_time::microsec_clock::local_time()){}

	void wait(double period)
	{				
		auto remaining = boost::posix_time::microseconds(static_cast<long>(period*1000000.0)) - (boost::posix_time::microsec_clock::local_time() - time_);
		if(remaining > boost::posix_time::microseconds(5000))
			boost::this_thread::sleep(remaining - boost::posix_time::microseconds(5000));
	}
private:
	boost::posix_time::ptime time_;
};

struct frame_consumer_device::implementation
{
	typedef safe_ptr<const read_frame> frame_type;
public:
	implementation(const video_format_desc& format_desc, const std::vector<safe_ptr<frame_consumer>>& consumers) : consumers_(consumers), fmt_(format_desc)
	{		
		std::vector<size_t> depths;
		boost::range::transform(consumers_, std::back_inserter(depths), std::mem_fn(&frame_consumer::buffer_depth));
		max_depth_ = *boost::range::max_element(depths);
		input_.set_capacity(3);
		executor_.start();
		executor_.begin_invoke([=]{tick();});
	}
					
	void tick()
	{
		boost::shared_future<frame_type> future_frame;
		input_.pop(future_frame);
		
		auto frame = future_frame.get();

		buffer_.push_back(frame);

		clock_sync clock;
		
		boost::range::for_each(consumers_, [&](const safe_ptr<frame_consumer>& consumer)
		{
			size_t offset = max_depth_ - consumer->buffer_depth();
			if(offset < buffer_.size())
				consumer->send(*(buffer_.begin() + offset));
		});
			
		frame_consumer::sync_mode sync = frame_consumer::ready;
		boost::range::for_each(consumers_, [&](const safe_ptr<frame_consumer>& consumer)
		{
			try
			{
				size_t offset = max_depth_ - consumer->buffer_depth();
				if(offset >= buffer_.size())
					return;

				if(consumer->synchronize() == frame_consumer::clock)
					sync = frame_consumer::clock;
			}
			catch(...)
			{
				CASPAR_LOG_CURRENT_EXCEPTION();
				boost::range::remove_erase(consumers_, consumer);
				CASPAR_LOG(warning) << "Removed consumer from frame_consumer_device.";
			}
		});
	
		if(sync != frame_consumer::clock)
			clock.wait(fmt_.period);

		if(buffer_.size() >= max_depth_)
			buffer_.pop_front();
	}

	void consume(boost::unique_future<frame_type>&& frame)
	{		
		input_.push(boost::shared_future<frame_type>(std::move(frame)));
	}

	tbb::concurrent_bounded_queue<boost::shared_future<frame_type>> input_;

	executor executor_;	

	size_t max_depth_;
	std::deque<safe_ptr<const read_frame>> buffer_;		

	std::vector<safe_ptr<frame_consumer>> consumers_;
	
	const video_format_desc& fmt_;
};

frame_consumer_device::frame_consumer_device(frame_consumer_device&& other) : impl_(std::move(other.impl_)){}
frame_consumer_device::frame_consumer_device(const video_format_desc& format_desc, const std::vector<safe_ptr<frame_consumer>>& consumers)
	: impl_(new implementation(format_desc, consumers)){}
void frame_consumer_device::consume(boost::unique_future<safe_ptr<const read_frame>>&& future_frame) { impl_->consume(std::move(future_frame)); }
}}