#include "audio.h"
#include <3ds.h>
#include <tremor/ivorbisfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>

void logf_(const char* fmt, ...);

namespace Audio
{
	// ---------------------------------------------------------------- muestras decodificadas
	// estado: 0 sin pedir, 1 en cola, 2 lista, 3 falló (la decodificación es sólo del hilo de audio)
	struct Sample { s16* data = nullptr; u32 frames = 0; int channels = 1; int rate = 22050; volatile int state = 0; };
	struct Event { bool loop = false; float volume = 1; std::vector<std::string> samples; };

	static bool available = false;
	static std::map<std::string, Event> events;
	static std::map<std::string, Sample> samples;
	static char status[64] = "sin iniciar";

	static Vec3 listenerPos, listenerForward(0, 1, 0);
	static float sfxVolume = 1.0f;

	void SetSfxVolume(float v) { sfxVolume = v; }

	static LightLock sampleLock;
	static std::vector<Sample*> decodeQueue;

	// pide la muestra; sólo la devuelve si ya está decodificada
	static Sample* getSample(const std::string& name)
	{
		LightLock_Lock(&sampleLock);
		Sample& s = samples[name];   // std::map: las direcciones no cambian al insertar
		if (s.state == 0)
		{
			s.state = 1;
			decodeQueue.push_back(&s);
		}
		LightLock_Unlock(&sampleLock);
		return s.state == 2 ? &s : nullptr;
	}

	// decodificación por partes: un trozo por llamada, para que la música nunca espere
	struct DecodeJob
	{
		Sample* sample = nullptr;
		OggVorbis_File vf;
		bool open = false;
		std::vector<s16> pcm;
		int channels = 1, rate = 22050;
	};
	static DecodeJob job;

	static void finishJob(bool ok)
	{
		Sample& s = *job.sample;
		if (job.open) { ov_clear(&job.vf); job.open = false; }
		s16* data = ok && !job.pcm.empty() ? (s16*)linearAlloc(job.pcm.size() * sizeof(s16)) : nullptr;
		if (!data)
			s.state = 3;
		else
		{
			memcpy(data, job.pcm.data(), job.pcm.size() * sizeof(s16));
			DSP_FlushDataCache(data, job.pcm.size() * sizeof(s16));
			s.channels = job.channels;
			s.rate = job.rate;
			s.frames = (u32)(job.pcm.size() / job.channels);
			s.data = data;
			s.state = 2;
		}
		job.sample = nullptr;
		std::vector<s16>().swap(job.pcm);
	}

	// devuelve true si queda trabajo pendiente
	static bool decodeStep()
	{
		if (!job.sample)
		{
			std::string name;
			LightLock_Lock(&sampleLock);
			if (!decodeQueue.empty())
			{
				job.sample = decodeQueue.front();
				decodeQueue.erase(decodeQueue.begin());
				for (auto& kv : samples)
					if (&kv.second == job.sample) { name = kv.first; break; }
			}
			LightLock_Unlock(&sampleLock);
			if (!job.sample) return false;
			std::string path = "romfs:/audio/sfx/" + name + ".ogg";
			FILE* f = fopen(path.c_str(), "rb");
			if (!f || ov_open(f, &job.vf, NULL, 0) < 0)
			{
				if (f) fclose(f);
				finishJob(false);
				return true;
			}
			job.open = true;
			vorbis_info* vi = ov_info(&job.vf, -1);
			job.channels = vi->channels;
			job.rate = (int)vi->rate;
			return true;
		}
		// unos 8 KB de PCM por paso
		char buf[4096];
		int section;
		for (int i = 0; i < 2; i++)
		{
			long n = ov_read(&job.vf, buf, sizeof(buf), &section);
			if (n <= 0) { finishJob(n == 0); return true; }
			job.pcm.insert(job.pcm.end(), (s16*)buf, (s16*)(buf + n));
		}
		return true;
	}

	// ---------------------------------------------------------------- voces (canales 1..23)
	static constexpr int FirstVoice = 1, LastVoice = 23;
	struct Voice
	{
		int handle = 0;
		ndspWaveBuf buf;
		bool positional = false;
		Vec3 position;
		float volume = 1;
		bool loop = false;
	};
	static Voice voices[24];
	static int nextHandle = 1;

	static void applyMix(int ch, const Voice& v)
	{
		float vol = v.volume * sfxVolume, pan = 0;
		if (v.positional)
		{
			// atenuación aproximada de los eventos 3D de FMOD: plena hasta 40, nada desde 400
			Vec3 d = v.position - listenerPos;
			float dist = d.Length();
			vol *= Calc::ClampedMap(dist, 40, 400, 1, 0);
			Vec3 right = Cross(listenerForward, Vec3::UnitZ()).Normalized();
			if (dist > 1) pan = Dot(d / dist, right) * 0.6f;
		}
		float mix[12] = {};
		mix[0] = vol * (pan > 0 ? 1 - pan : 1);
		mix[1] = vol * (pan < 0 ? 1 + pan : 1);
		ndspChnSetMix(ch, mix);
	}

	static int startVoice(Sample* s, float volume, bool loop, const Vec3* position)
	{
		if (!available || !s) return 0;
		int ch = -1;
		for (int i = FirstVoice; i <= LastVoice; i++)
			if (!voices[i].handle || !ndspChnIsPlaying(i)) { ch = i; break; }
		if (ch < 0) return 0;
		Voice& v = voices[ch];
		ndspChnWaveBufClear(ch);
		ndspChnReset(ch);
		ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
		ndspChnSetRate(ch, (float)s->rate);
		ndspChnSetFormat(ch, s->channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
		v.handle = nextHandle++;
		v.positional = position != nullptr;
		if (position) v.position = *position;
		v.volume = volume;
		v.loop = loop;
		applyMix(ch, v);
		memset(&v.buf, 0, sizeof(v.buf));
		v.buf.data_vaddr = s->data;
		v.buf.nsamples = s->frames;
		v.buf.looping = loop;
		ndspChnWaveBufAdd(ch, &v.buf);
		return v.handle;
	}

	static Voice* find(int handle, int* chOut = nullptr)
	{
		if (!handle) return nullptr;
		for (int i = FirstVoice; i <= LastVoice; i++)
			if (voices[i].handle == handle) { if (chOut) *chOut = i; return &voices[i]; }
		return nullptr;
	}

	int Play(const char* event, const Vec3* position)
	{
		if (!available) return 0;
		auto it = events.find(event);
		if (it == events.end() || it->second.samples.empty()) return 0;
		const Event& e = it->second;
		const std::string& name = e.samples[rand() % e.samples.size()];
		return startVoice(getSample(name), e.volume, e.loop, position);
	}

	static std::string pendingVoice;
	static u64 pendingVoiceTick;

	int PlayVoice(const char* voice)
	{
		// event:/sfx/ui/dialog/<personaje>/<línea>: la muestra se llama como la línea
		const char* slash = strrchr(voice, '/');
		std::string name = slash ? slash + 1 : voice;
		Sample* s = getSample(name);
		if (s) return startVoice(s, 1.0f, false, nullptr);
		pendingVoice = name;             // suena en Update() apenas esté lista
		pendingVoiceTick = svcGetSystemTick();
		return 0;
	}

	void PreloadVoice(const char* voice)
	{
		const char* slash = strrchr(voice, '/');
		getSample(slash ? slash + 1 : voice);
	}

	void Stop(int handle)
	{
		int ch;
		if (Voice* v = find(handle, &ch)) { ndspChnWaveBufClear(ch); v->handle = 0; }
	}

	bool IsPlaying(int handle)
	{
		int ch;
		return find(handle, &ch) && ndspChnIsPlaying(ch);
	}

	void SetPosition(int handle, Vec3 position)
	{
		if (Voice* v = find(handle)) v->position = position;
	}

	void StopAll()
	{
		for (int i = FirstVoice; i <= LastVoice; i++)
			if (voices[i].handle) { ndspChnWaveBufClear(i); voices[i].handle = 0; }
	}

	void SetListener(Vec3 position, Vec3 forward)
	{
		listenerPos = position;
		listenerForward = forward;
	}

	void Update()
	{
		if (!available) return;
		if (!pendingVoice.empty())
		{
			LightLock_Lock(&sampleLock);
			Sample& s = samples[pendingVoice];
			LightLock_Unlock(&sampleLock);
			if (s.state == 2) { startVoice(&s, 1.0f, false, nullptr); pendingVoice.clear(); }
			else if (s.state == 3 || svcGetSystemTick() - pendingVoiceTick > SYSCLOCK_ARM11 / 2) pendingVoice.clear();
		}
		for (int i = FirstVoice; i <= LastVoice; i++)
		{
			Voice& v = voices[i];
			if (!v.handle) continue;
			if (!ndspChnIsPlaying(i)) { v.handle = 0; continue; }
			if (v.positional) applyMix(i, v);
		}
	}

	// ---------------------------------------------------------------- música (canal 0, hilo aparte)
	static constexpr int MusicChannel = 0;
	static constexpr int MusicBufFrames = 4096;
	static constexpr int MusicBufs = 8;   // ~1 s a 32 kHz
	static ndspWaveBuf musicBufs[MusicBufs];
	static s16* musicData;
	static Thread musicThread;
	static LightLock musicLock;
	static volatile bool musicQuit = false;
	static OggVorbis_File musicFile;
	static bool musicOpen = false;
	static std::string musicName, musicRequest;
	static volatile bool musicChange = false;
	static float musicVolume = 1.0f;

	static bool fillMusic(ndspWaveBuf& b)
	{
		s16* out = (s16*)b.data_vaddr;
		int want = MusicBufFrames * 2 * sizeof(s16);
		int got = 0, section;
		while (got < want)
		{
			long n = ov_read(&musicFile, (char*)out + got, want - got, &section);
			if (n <= 0)
			{
				// fin de la pista: vuelve a empezar (los eventos de música del original repiten)
				if (ov_pcm_seek(&musicFile, 0) != 0) break;
				continue;
			}
			got += (int)n;
		}
		if (got <= 0) return false;
		b.nsamples = got / (2 * sizeof(s16));
		DSP_FlushDataCache(out, got);
		ndspChnWaveBufAdd(MusicChannel, &b);
		return true;
	}

	static void openMusic(const std::string& name)
	{
		ndspChnWaveBufClear(MusicChannel);
		if (musicOpen) { ov_clear(&musicFile); musicOpen = false; }
		musicName = name;
		if (name.empty()) return;
		std::string path = "romfs:/audio/music/" + name + ".ogg";
		FILE* f = fopen(path.c_str(), "rb");
		if (!f || ov_open(f, &musicFile, NULL, 0) < 0)
		{
			if (f) fclose(f);
			logf_("no se pudo abrir %s\n", path.c_str());
			return;
		}
		musicOpen = true;
		vorbis_info* vi = ov_info(&musicFile, -1);
		ndspChnReset(MusicChannel);
		ndspChnSetInterp(MusicChannel, NDSP_INTERP_LINEAR);
		ndspChnSetRate(MusicChannel, (float)vi->rate);
		ndspChnSetFormat(MusicChannel, vi->channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
		float mix[12] = { musicVolume, musicVolume };
		ndspChnSetMix(MusicChannel, mix);
		for (int i = 0; i < MusicBufs; i++)
		{
			musicBufs[i].status = NDSP_WBUF_DONE;
			fillMusic(musicBufs[i]);
		}
	}

	static void musicThreadMain(void*)
	{
		while (!musicQuit)
		{
			if (musicChange)
			{
				LightLock_Lock(&musicLock);
				std::string req = musicRequest;
				musicChange = false;
				LightLock_Unlock(&musicLock);
				openMusic(req);
			}
			if (musicOpen)
				for (auto& b : musicBufs)
					if (b.status == NDSP_WBUF_DONE)
						fillMusic(b);
			// con la música al día, un trozo de algún efecto pendiente
			if (decodeStep())
				svcSleepThread(1 * 1000 * 1000);    // 1 ms entre trozos
			else
				svcSleepThread(10 * 1000 * 1000);   // 10 ms
		}
		if (musicOpen) { ov_clear(&musicFile); musicOpen = false; }
	}

	void PlayMusic(const char* name)
	{
		if (!available) return;
		LightLock_Lock(&musicLock);
		musicRequest = name ? name : "";
		musicChange = true;
		LightLock_Unlock(&musicLock);
	}

	void StopMusic() { PlayMusic(""); }

	void SetMusicVolume(float v)
	{
		musicVolume = v;
		float mix[12] = { v, v };
		if (available) ndspChnSetMix(MusicChannel, mix);
	}

	// ---------------------------------------------------------------- inicio
	static void loadEvents()
	{
		FILE* f = fopen("romfs:/audio/events.txt", "r");
		if (!f) return;
		char line[512];
		while (fgets(line, sizeof(line), f))
		{
			char* parts[4] = {};
			int n = 0;
			for (char* tok = strtok(line, "|\r\n"); tok && n < 4; tok = strtok(nullptr, "|\r\n"))
				parts[n++] = tok;
			if (n < 4) continue;
			Event e;
			e.loop = atoi(parts[1]) != 0;
			e.volume = (float)atof(parts[2]);
			for (char* s = strtok(parts[3], ","); s; s = strtok(nullptr, ","))
				e.samples.push_back(s);
			events[parts[0]] = e;
		}
		fclose(f);
	}

	bool Init()
	{
		if (R_FAILED(ndspInit()))
		{
			snprintf(status, sizeof(status), "sin DSP (falta dspfirm.cdc)");
			logf_("audio: ndspInit falló; hace falta sdmc:/3ds/dspfirm.cdc\n");
			return false;
		}
		available = true;
		ndspSetOutputMode(NDSP_OUTPUT_STEREO);
		loadEvents();
		LightLock_Init(&sampleLock);
		// pide en segundo plano todas las muestras de los eventos (menos el ambiente, que ya se pidió al sonar)
		for (auto& kv : events)
			for (auto& name : kv.second.samples)
				getSample(name);

		musicData = (s16*)linearAlloc(MusicBufs * MusicBufFrames * 2 * sizeof(s16));
		for (int i = 0; i < MusicBufs; i++)
		{
			memset(&musicBufs[i], 0, sizeof(ndspWaveBuf));
			musicBufs[i].data_vaddr = musicData + i * MusicBufFrames * 2;
			musicBufs[i].status = NDSP_WBUF_DONE;
		}
		LightLock_Init(&musicLock);

		// la decodificación va en el segundo núcleo (en la Old 3DS hay que pedirlo)
		APT_SetAppCpuTimeLimit(40);
		s32 prio = 0x30;
		svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
		musicThread = threadCreate(musicThreadMain, nullptr, 32 * 1024, prio - 1, 1, false);
		const char* core = "nucleo 1";
		if (!musicThread)
		{
			musicThread = threadCreate(musicThreadMain, nullptr, 32 * 1024, prio - 1, 0, false);
			core = "nucleo 0";
		}
		snprintf(status, sizeof(status), "ok, %d eventos, musica en %s", (int)events.size(), core);
		logf_("audio: %s\n", status);
		return true;
	}

	bool Available() { return available; }
	const char* Status() { return status; }

	void Exit()
	{
		if (!available) return;
		musicQuit = true;
		if (musicThread) { threadJoin(musicThread, U64_MAX); threadFree(musicThread); }
		ndspExit();
		available = false;
	}
}
