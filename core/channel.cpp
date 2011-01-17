#include "StdAfx.h"

#include "channel.h"

#include "consumer/frame_consumer_device.h"

#include "mixer/frame/draw_frame.h"
#include "mixer/frame_mixer_device.h"

#include "producer/layer.h"

#include <common/concurrency/executor.h>

#include <boost/range/algorithm_ext/erase.hpp>

#include <tbb/parallel_for.h>

#include <map>
#include <memory>

namespace caspar { namespace core {

struct channel::implementation : boost::noncopyable
{					
	const safe_ptr<frame_mixer_device> processor_device_;
	frame_consumer_device consumer_device_;
						
	std::map<int, layer> layers_;		

	const video_format_desc format_desc_;

	mutable executor executor_;

public:
	implementation(const video_format_desc& format_desc)  
		: format_desc_(format_desc)
		, processor_device_(frame_mixer_device(format_desc))
		, consumer_device_(format_desc)
	{
		executor_.start();
		executor_.begin_invoke([=]{tick();});
	}
					
	void tick()
	{		
		auto drawed_frame = draw();
		auto processed_frame = processor_device_->process(std::move(drawed_frame));
		consumer_device_.consume(std::move(processed_frame));

		executor_.begin_invoke([=]{tick();});
	}
	
	safe_ptr<draw_frame> draw()
	{	
		std::vector<safe_ptr<draw_frame>> frames(layers_.size(), draw_frame::empty());
		tbb::parallel_for(tbb::blocked_range<size_t>(0, frames.size()), 
		[&](const tbb::blocked_range<size_t>& r)
		{
			auto it = layers_.begin();
			std::advance(it, r.begin());
			for(size_t i = r.begin(); i != r.end(); ++i, ++it)
				frames[i] = it->second.receive();
		});		
		boost::range::remove_erase(frames, draw_frame::eof());
		boost::range::remove_erase(frames, draw_frame::empty());
		return draw_frame(frames);
	}

	// Consumers
	void add(int index, const safe_ptr<frame_consumer>& consumer)
	{
		consumer_device_.add(index, consumer);
	}
	
	void remove(int index)
	{
		consumer_device_.remove(index);
	}

	// Layers and Producers
	void set_video_gain(int index, double value)
	{
		begin_invoke_layer(index, std::bind(&layer::set_video_gain, std::placeholders::_1, value));
	}

	void set_video_opacity(int index, double value)
	{
		begin_invoke_layer(index, std::bind(&layer::set_video_opacity, std::placeholders::_1, value));
	}

	void set_audio_gain(int index, double value)
	{
		begin_invoke_layer(index, std::bind(&layer::set_audio_gain, std::placeholders::_1, value));
	}

	void load(int index, const safe_ptr<frame_producer>& producer, bool play_on_load)
	{
		producer->initialize(processor_device_);
		executor_.begin_invoke([=]
		{
			auto it = layers_.insert(std::make_pair(index, layer(index))).first;
			it->second.load(producer, play_on_load);
		});
	}
			
	void preview(int index, const safe_ptr<frame_producer>& producer)
	{
		producer->initialize(processor_device_);
		executor_.begin_invoke([=]
		{
			auto it = layers_.insert(std::make_pair(index, layer(index))).first;
			it->second.preview(producer);
		});
	}

	void pause(int index)
	{		
		begin_invoke_layer(index, std::mem_fn(&layer::pause));
	}

	void play(int index)
	{		
		begin_invoke_layer(index, std::mem_fn(&layer::play));
	}

	void stop(int index)
	{		
		begin_invoke_layer(index, std::mem_fn(&layer::stop));
	}

	void clear(int index)
	{
		executor_.begin_invoke([=]
		{			
			auto it = layers_.find(index);
			if(it != layers_.end())
			{
				it->second.clear();		
				layers_.erase(it);
			}
		});
	}
		
	void clear()
	{
		executor_.begin_invoke([=]
		{			
			layers_.clear();
		});
	}		

	template<typename F>
	void begin_invoke_layer(int index, F&& func)
	{
		executor_.begin_invoke([=]
		{
			auto it = layers_.find(index);
			if(it != layers_.end())
				func(it->second);	
		});
	}

	boost::unique_future<safe_ptr<frame_producer>> foreground(int index) const
	{
		return executor_.begin_invoke([=]() -> safe_ptr<frame_producer>
		{			
			auto it = layers_.find(index);
			return it != layers_.end() ? it->second.foreground() : frame_producer::empty();
		});
	}
	
	boost::unique_future<safe_ptr<frame_producer>> background(int index) const
	{
		return executor_.begin_invoke([=]() -> safe_ptr<frame_producer>
		{
			auto it = layers_.find(index);
			return it != layers_.end() ? it->second.background() : frame_producer::empty();
		});
	};
};

channel::channel(channel&& other) : impl_(std::move(other.impl_)){}
channel::channel(const video_format_desc& format_desc) : impl_(new implementation(format_desc)){}

void channel::add(int index, const safe_ptr<frame_consumer>& consumer){impl_->add(index, consumer);}
void channel::remove(int index){impl_->remove(index);}

void channel::set_video_gain(int index, double value){impl_->set_video_gain(index, value);}
void channel::set_video_opacity(int index, double value){impl_->set_video_opacity(index, value);}
void channel::set_audio_gain(int index, double value){impl_->set_audio_gain(index, value);}
void channel::load(int index, const safe_ptr<frame_producer>& producer, bool play_on_load){impl_->load(index, producer, play_on_load);}
void channel::preview(int index, const safe_ptr<frame_producer>& producer){impl_->preview(index, producer);}
void channel::pause(int index){impl_->pause(index);}
void channel::play(int index){impl_->play(index);}
void channel::stop(int index){impl_->stop(index);}
void channel::clear(int index){impl_->clear(index);}
void channel::clear(){impl_->clear();}
boost::unique_future<safe_ptr<frame_producer>> channel::foreground(int index) const{	return impl_->foreground(index);}
boost::unique_future<safe_ptr<frame_producer>> channel::background(int index) const{return impl_->background(index);}
const video_format_desc& channel::get_video_format_desc() const{	return impl_->format_desc_;}

}}