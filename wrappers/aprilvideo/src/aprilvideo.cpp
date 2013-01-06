/************************************************************************************
This source file is part of the Theora Video Playback Library
For latest info, see http://libtheoraplayer.sourceforge.net/
*************************************************************************************
Copyright (c) 2008-2012 Kresimir Spes (kspes@cateia.com)
This program is free software; you can redistribute it and/or modify it under
the terms of the BSD license: http://www.opensource.org/licenses/bsd-license.php
*************************************************************************************/
#define THEORAUTIL_NOMACROS
#include <theoraplayer/TheoraException.h>
#undef exception_cls
#include <theoraplayer/TheoraDataSource.h>
#include <theoraplayer/TheoraPlayer.h>
#include <theoraplayer/TheoraTimer.h>
#include <april/RenderSystem.h>
#include <april/Texture.h>
#include <aprilui/aprilui.h>
#include <aprilui/Dataset.h>
#include <aprilui/NullImage.h>
#include <aprilui/Texture.h>
#include <xal/AudioManager.h>
#include <xal/Player.h>
#include "aprilvideo.h"

namespace aprilvideo
{
	static TheoraVideoManager* gVideoManager = NULL;
	static int gRefCount = 0, gNumWorkerThreads = 1;
	hstr logTag = "aprilvideo";

	static int _nextPow2(int x)
	{
		int y;
		for (y = 1; y < x; y *= 2);
		return y;
	}

	class AudioVideoTimer : public TheoraTimer
	{
		float mSyncOffset;
		xal::Player* mPlayer;
		float mT;
		bool mDisabledAudio;
	public:
		AudioVideoTimer(xal::Player* player, float sync_offset)
		{
			mSyncOffset = sync_offset;
			mPlayer = player;
			mT = 0;
			static hstr audiosystem = xal::mgr->getName(); // Disabled" audio system doesn't sync audio & video
			mDisabledAudio = (audiosystem == "Disabled");
		}
		
		void update(float time_increase)
		{
			if (!mDisabledAudio)
			{
				if (mPlayer->isPlaying())
					mTime = mPlayer->getTimePosition() - mSyncOffset;
				else
					mTime += time_increase;
			}
			else
			{
				mT += time_increase;
				mTime = mT - mSyncOffset;
			}
		}
	};
	

	VideoObject::VideoObject(chstr name, grect rect) : aprilui::ImageBox(name, rect)
	{
		mUseAlpha = 0;
		mPrevDoneFlag = 0;
		mLoop = 1;
		mSpeed = 1.0f;
		mClip = NULL;
		mVideoImage = NULL;
		mTexture = NULL;
		mTimer = NULL;
		mSound = NULL;
		mAudioPlayer = NULL;
		mAudioSyncOffset = 0;
		gRefCount++;
		mAlphaPauseTreshold = 0;
		mPrevFrameNumber = 0;
		mSeeked = 0;
		if (!gVideoManager) gVideoManager = new TheoraVideoManager(gNumWorkerThreads);
	}
	
	aprilui::Object* VideoObject::createInstance(chstr name, grect rect)
	{
		return new VideoObject(name, rect);
	}

	void VideoObject::notifyEvent(chstr name, void* params)
	{
		if (name == "AttachToObject")
		{
			if (mImage != mVideoImage)
			{
				mImage = mDataset->getImage("null");
			}
		}
		ImageBox::notifyEvent(name, params);
	}
	
	void VideoObject::destroyResources()
	{
		if (mVideoImage)
		{
			delete mVideoImage;
			mVideoImage = NULL;
			mImage = mDataset->getImage("null");
		}
		if (mTexture)
		{
			delete mTexture;
			mTexture = NULL;
		}
		if (mClip)
		{
			gVideoManager->destroyVideoClip(mClip);
			mClip = NULL;
		}
		if (mAudioPlayer)
		{
			xal::mgr->destroyPlayer(mAudioPlayer);
			mAudioPlayer = NULL;
		}
		if (mSound)
		{
			xal::mgr->destroySound(mSound);
			mSound = NULL;
		}
		
		if (mTimer)
		{
			delete mTimer;
			mTimer = NULL;
		}
	}

	VideoObject::~VideoObject()
	{
		gRefCount--;
		destroyResources();
		if (gRefCount <= 0 && gVideoManager)
		{
			delete gVideoManager;
			gVideoManager = NULL;
		}
	}
	
	void VideoObject::update(float k)
	{
		ImageBox::update(k);
		if (mClip == NULL && mClipName != "")
		{
			destroyResources();
			hstr path = mDataset->getFilePath() + "/video/" + mClipName;
			try
			{
				TheoraOutputMode mode;
				if (april::rendersys->getName() == "OpenGL") mode = mUseAlpha ? TH_RGBA : TH_RGB;
				else                                         mode = mUseAlpha ? TH_BGRA : TH_BGRX;

				mClip = gVideoManager->createVideoClip(path, mode, 16);
			}
			catch (_TheoraGenericException& e)
			{
				throw hl_exception(e.getErrorText());
			}
			if (mClip->getWidth() == 0) throw hl_exception("Failed to load video file: " + path);
			mClip->setAutoRestart(mLoop);
			
			float w = mClip->getWidth(), h = mClip->getHeight();
			april::Texture* tex = april::rendersys->createTexture(_nextPow2(w), _nextPow2(h), april::Texture::FORMAT_ARGB);
			mTexture = new aprilui::Texture(tex->getFilename(), tex);
			mVideoImage = new aprilui::Image(mTexture, "video_img", grect(0, 0, w, h));
			mClip->waitForCache(4 / 16.0f, 0.5f);
			
			if (mAudioName != "")
			{
				if (!xal::mgr->hasCategory("video"))
				{
					xal::mgr->createCategory("video", xal::ON_DEMAND, xal::DISK);
				}
				mSound = xal::mgr->createSound(mDataset->getFilePath() + "/video/" + mAudioName, "video");

				mAudioPlayer = xal::mgr->createPlayer(mAudioName.replace(".ogg", ""));
				mTimer = new AudioVideoTimer(mAudioPlayer, mAudioSyncOffset);
				mClip->setTimer(mTimer);
				
				mAudioPlayer->play();
			}
			else if (mSpeed != 1.0f) mClip->setPlaybackSpeed(mSpeed);
		}

		if (mClip)
		{
			if (!mLoop)
			{
				bool done = mClip->isDone();
				if (mPrevDoneFlag == 0 && done == 1) triggerEvent("PlaybackDone");
				mPrevDoneFlag = done;
			}
			mClip->update(k);
			//mClip->decodedAudioCheck();
			bool should_pause = mAlphaPauseTreshold == 0 ? !isVisible() : getAlpha() <= mAlphaPauseTreshold;
			if (should_pause && !mClip->isPaused()) mClip->pause();
			else if (!should_pause && mClip->isPaused()) mClip->play();
			
			TheoraVideoFrame* f = mClip->getNextFrame();
			if (f)
			{
				mImage = mVideoImage;
				grect r = mImage->getSrcRect();
				r.w = f->getWidth();
				r.h = f->getHeight();
				mImage->setSrcRect(r);
				if (april::rendersys->getName() == "DirectX9") mTexture->getRenderTexture()->clear();
				mTexture->getRenderTexture()->blit(0, 0, f->getBuffer(), r.w, r.h, 3 + mUseAlpha, 0, 0, r.w, r.h);
				mClip->popFrame();
				if (mLoop)
				{
					int number = f->getFrameNumber();
					if (mSeeked) mSeeked = 0;
					else if (number < mPrevFrameNumber)
					{
						triggerEvent("PlaybackDone");
					}
					mPrevFrameNumber = number;
				}
			}
		}
	}
	
	void VideoObject::setAlphaTreshold(int treshold)
	{
		mAlphaPauseTreshold = hclamp(treshold, 0, 255);
	}
	
	bool VideoObject::setProperty(chstr name,chstr value)
	{
		if      (name == "video") mClipName = value;
		else if (name == "video_alpha") mUseAlpha = value;
		else if (name == "alpha_pause_treshold") setAlphaTreshold(value);
		else if (name == "loop")  mLoop = value;
		else if (name == "speed") mSpeed = value;
		else if (name == "time")
		{
			if (!mClip && mClipName != "") update(0); // try to create the clip if it hasn't been created already
			if (mClip)
			{
				float time = value;
				mSeeked = 1;
				mClip->seek(time);
			}
			else hlog::warn(logTag, "VideoObject ignoring 'time' param, mClip is NULL");
		}
		else if (name == "audio")
		{
			mAudioName = value;
		}
		else if (name == "sync_offset")
		{
			mAudioSyncOffset = value;
		}
		else return aprilui::ImageBox::setProperty(name, value);
		return 1;
	}
	
	hstr VideoObject::getProperty(chstr name, bool* property_exists)
	{
		*property_exists = true;
		if      (name == "video") return mClipName;
		else if (name == "video_alpha") return mUseAlpha ? "1" : "0";
		else if (name == "alpha_pause_treshold") return mAlphaPauseTreshold;
		else if (name == "loop")  return mLoop ? "1" : "0";
		else if (name == "speed") return mSpeed;
		else if (name == "time") return mClip ? hstr(mClip->getTimePosition()) : hstr("0");
		else if (name == "duration") return mClip ? hstr(mClip->getDuration()) : hstr("0");
		else if (name == "audio")  return mAudioName;
		else if (name == "sync_offset")  return mAudioSyncOffset;
		*property_exists = false;
		return ImageBox::getProperty(name, property_exists);
	}

	
	static void theoraLogMessage(std::string log)
	{
		hlog::write("aprilvideo", log);
	}
	
	void init(int num_worker_threads)
	{
		TheoraVideoManager::setLogFunction(theoraLogMessage);
		gNumWorkerThreads = num_worker_threads;
		APRILUI_REGISTER_OBJECT_TYPE(VideoObject);
	}

	void destroy()
	{
	
	}
}