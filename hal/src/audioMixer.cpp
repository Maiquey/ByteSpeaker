// Incomplete implementation of an audio mixer. Search for "REVISIT" to find things
// which are left as incomplete.
// Note: Generates low latency audio on BeagleBone Black; higher latency found on host.
#include <fstream>
#include <iostream>
#include <pthread.h>

#include "hal/audioMixer.hpp"


AudioMixer::AudioMixer(LanguageManager *languageManagerReference) 
	: custom1Sound{0, nullptr}, custom2Sound{0, nullptr}
{
	languageManager = languageManagerReference;
	setVolume(DEFAULT_VOLUME);

	// Initialize the sound-bite
	soundBite.pSound = NULL;
	soundBite.location = -1;

	// Open the PCM output
	int err = snd_pcm_open(&handle, "default", SND_PCM_STREAM_PLAYBACK, 0);
	if (err < 0) {
		std::cerr << "Playback open error: " << snd_strerror(err) << std::endl;
		exit(EXIT_FAILURE);
	}

	// Configure parameters of PCM output
	err = snd_pcm_set_params(handle,
			SND_PCM_FORMAT_S16_LE,
			SND_PCM_ACCESS_RW_INTERLEAVED,
			NUM_CHANNELS,
			SAMPLE_RATE,
			1,			// Allow software resampling
			50000);		// 0.05 seconds per buffer
	if (err < 0) {
		std::cerr << "Playback open error: " << snd_strerror(err) << std::endl;
		exit(EXIT_FAILURE);
	}

	// Allocate this software's playback buffer to be the same size as the
	// the hardware's playback buffers for efficient data transfers.
	// ..get info on the hardware buffers:
 	unsigned long unusedBufferSize = 0;
	snd_pcm_get_params(handle, &unusedBufferSize, &playbackBufferSize);
	// ..allocate playback buffer:
	playbackBuffer = new short[playbackBufferSize];

	// SET DEFAULT MESSAGES (Pre-recorded)
	readWaveFileIntoMemory(languageManager->getWavFilename(ENGLISH), ENGLISH);
	readWaveFileIntoMemory(languageManager->getWavFilename(FRENCH), FRENCH);
	readWaveFileIntoMemory(languageManager->getWavFilename(GERMAN), GERMAN);
	readWaveFileIntoMemory(languageManager->getWavFilename(SPANISH), SPANISH);
	readWaveFileIntoMemory(languageManager->getWavFilename(CHINESE), CHINESE);

	// Check for custom messages
	std::string custom1Filename = languageManager->getWavFilename(CUSTOM_1);
    std::ifstream custom1File(custom1Filename);
    if(custom1File.good()) {
		readWaveFileIntoMemory(languageManager->getWavFilename(CUSTOM_1), CUSTOM_1);
    }

	std::string custom2Filename = languageManager->getWavFilename(CUSTOM_2);
    std::ifstream custom2File(custom2Filename);
    if(custom2File.good()) {
		readWaveFileIntoMemory(languageManager->getWavFilename(CUSTOM_2), CUSTOM_2);
    }

	// Launch playback thread:
	playbackThreadId = std::thread([this]() {
        this->playbackThread(nullptr);
    });

	// In order to have the audio playback be smooth WHILE piper is translating messages, we must
	// set the priority of the playback thread to be higher
	pthread_t nativeHandle = playbackThreadId.native_handle();
    struct sched_param params;
    params.sched_priority = 2;

    err = pthread_setschedparam(nativeHandle, SCHED_FIFO, &params);
    if (err != 0) {
        std::cerr << "Failed to set priority for playback thread: " << strerror(err) << std::endl;
		std::cout << "Please check that you have run the program with sudo." << std::endl;
    }
}


// Client code must call AudioMixer_freeWaveFileData to free dynamically allocated data.
void AudioMixer::readWaveFileIntoMemory(std::string fileName, enum Language language)
{
	wavedata_t *fetchedSound;

	pthread_mutex_lock(&audioMutex);

	switch(language) {
		case FRENCH:
			fetchedSound = &frenchSound;
			break;
		case GERMAN:
			fetchedSound = &germanSound;
			break;
		case SPANISH:
			fetchedSound = &spanishSound;
			break;
		case CHINESE:
			fetchedSound = &chineseSound;
			break;
		case CUSTOM_1:
			fetchedSound = &custom1Sound;
			break;
		case CUSTOM_2:
			fetchedSound = &custom2Sound;
			break;
		default:
			fetchedSound = &englishSound;
			break;
	}
	assert(fetchedSound);

	pthread_mutex_lock(&fileAccessMutex);
	// The PCM data in a wave file starts after the header:
	const int PCM_DATA_OFFSET = 44;

	std::fstream file(fileName, std::ios::in);

	if(!file.is_open()) {
		std::cerr << "ERROR: Unable to open file " << fileName << std::endl;
		exit(EXIT_FAILURE);
	}

	file.seekg(0, std::ios::end);
	std::streampos sizeInBytes = file.tellg();
	file.seekg(0, std::ios::beg);
	fetchedSound->numSamples = sizeInBytes / SAMPLE_SIZE;

	file.seekg(PCM_DATA_OFFSET);
	std::streampos pcmDataSize = sizeInBytes - static_cast<std::streampos>(PCM_DATA_OFFSET);
	fetchedSound->numSamples = pcmDataSize / SAMPLE_SIZE;

	// Allocate space to hold all PCM data
	fetchedSound->pData = new short[fetchedSound->numSamples];
	if (!fetchedSound->pData) {
		std::cerr << "ERROR: Unable to allocate " << sizeInBytes << " bytes for file "
			<< fileName << "." << std::endl;
		exit(EXIT_FAILURE);
	}

	file.read(reinterpret_cast<char*>(fetchedSound->pData), SAMPLE_SIZE * fetchedSound->numSamples);
	if (!file) {
		std::cerr << "ERROR: Unable to read " << fetchedSound->numSamples << " samples from file "
			<< fileName << "." << std::endl;
		exit(EXIT_FAILURE);
	}

	file.close();

	pthread_mutex_unlock(&fileAccessMutex);
	pthread_mutex_unlock(&audioMutex);
}

void AudioMixer::freeWaveFileData(wavedata_t *pSound)
{
	pthread_mutex_lock(&audioMutex);
	pSound->numSamples = 0;
	delete[] pSound->pData;
	pSound->pData = NULL;
	pthread_mutex_unlock(&audioMutex);
}

void AudioMixer::queueSound(enum Language language)
{
	// Check if we are stopping, if we are, cannot queue sound
	if(stopping) {
		return;
	}

	/*
	 * Since this may be called by other threads, and there is a thread
	 * processing the soundBites[] array, we must ensure access is threadsafe.
	 */
    pthread_mutex_lock(&audioMutex);

	// Fetch the correct sound based on desired language
	wavedata_t *fetchedSound;
	switch(language) {
		case FRENCH:
			fetchedSound = &frenchSound;
			break;
		case GERMAN:
			fetchedSound = &germanSound;
			break;
		case SPANISH:
			fetchedSound = &spanishSound;
			break;
		case CHINESE:
			fetchedSound = &chineseSound;
			break;
		case CUSTOM_1:
			fetchedSound = &custom1Sound;
			break;
		case CUSTOM_2:
			fetchedSound = &custom2Sound;
			break;
		default:
			fetchedSound = &englishSound;
			break;
	}

	// Ensure we are only being asked to play "good" sounds:
	if(fetchedSound->numSamples <= 0 || !fetchedSound->pData) {
		std::cout << "ERROR: Sound file is empty or not loaded." << std::endl;
		pthread_mutex_unlock(&audioMutex);
		return;
	}

        /*
        * Place the new sound file into that slot.
	    * Note: You are only copying a pointer, not the entire data of the wave file!
        */
	soundBite.pSound = fetchedSound;
	soundBite.location = 0;
	pthread_mutex_unlock(&audioMutex);
	return;
}

AudioMixer::~AudioMixer() {
	std::cout << "Stopping audio..." << std::endl;

	// Stop the PCM generation thread
	stopping = true;
	playbackThreadId.join();

	// Shutdown the PCM output, allowing any pending sound to play out (drain)
	snd_pcm_drain(handle);
	snd_pcm_close(handle);

	// Free playback buffer
	// (note that any wave files read into wavedata_t records must be freed
	//  in addition to this by calling AudioMixer_freeWaveFileData() on that struct.)
	delete[] playbackBuffer;
	playbackBuffer = NULL;

	// Free all sound here
	delete[] englishSound.pData;
	delete[] frenchSound.pData;
	delete[] germanSound.pData;
	delete[] spanishSound.pData;
	delete[] chineseSound.pData;

	// Custom sounds might not be loaded, so need to check for null before we free
	if(custom1Sound.pData) {
		delete[] custom1Sound.pData;
	}
	if(custom2Sound.pData) {
		delete[] custom2Sound.pData;
	}

	std::cout << "Done stopping audio..." << std::endl;
	fflush(stdout);
}


int AudioMixer::getVolume()
{
	// Return the cached volume; good enough unless someone is changing
	// the volume through other means and the cached value is out of date.
	return volume;
}

// Function copied from:
// http://stackoverflow.com/questions/6787318/set-alsa-master-volume-from-c-code
// Written by user "trenki".
void AudioMixer::setVolume(int newVolume)
{
	// Ensure volume is reasonable; If so, cache it for later getVolume() calls.
	if (newVolume < 0 || newVolume > AUDIOMIXER_MAX_VOLUME) {
		std::cout << "ERROR: Volume must be between 0 and 100." << std::endl;
		return;
	}
	volume = newVolume;

    long min, max;
    snd_mixer_t *volHandle;
    snd_mixer_selem_id_t *sid;
    const char *card = "default";
    const char *selem_name = "PCM";

    snd_mixer_open(&volHandle, 0);
    snd_mixer_attach(volHandle, card);
    snd_mixer_selem_register(volHandle, NULL, NULL);
    snd_mixer_load(volHandle);

    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, selem_name);
    snd_mixer_elem_t* elem = snd_mixer_find_selem(volHandle, sid);

    snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
    snd_mixer_selem_set_playback_volume_all(elem, volume * max / 100);

    snd_mixer_close(volHandle);
}


// Fill the `buff` array with new PCM values to output.
//    `buff`: buffer to fill with new PCM data from sound bites.
//    `size`: the number of values to store into playbackBuffer
void AudioMixer::fillPlaybackBuffer(short *buff, int size)
{
    // Set up a buffer of integers, so we can do calculations without worrying about overflow
	int *overflowBuff = new int[size];
    memset(overflowBuff, 0, sizeof(int)*size);

    memset(buff, 0, sizeof(short)*size);

    pthread_mutex_lock(&audioMutex);

	bool soundFinished = false;
	// Check if sound is waiting to be played
	if (soundBite.pSound != NULL) {
		// Check if we have less samples left than requested buffer fill size
		int samplesLeft = soundBite.pSound->numSamples - soundBite.location;
		if(samplesLeft <= size) {
			for (int ind=0; ind<samplesLeft; ind++) {
				overflowBuff[ind] += soundBite.pSound->pData[soundBite.location + ind];
			}
			soundFinished = true;
		}
		// Otherwise, copy full size amount, also <= above means we dont need to check for finished sound
		else {
			for (int ind=0; ind<size; ind++) {
				overflowBuff[ind] += soundBite.pSound->pData[soundBite.location + ind];
			}
			soundBite.location += size;
		}
		// If the sound is finished, "free" it
		if (soundFinished) {
			soundBite.pSound = NULL;
			soundBite.location = -1;
		}
	}

    pthread_mutex_unlock(&audioMutex);

    // Now, convert int buffer to short buffer, using trimming for overflows
    for (int ind=0; ind<size; ind++) {
        int val = overflowBuff[ind];
        buff[ind] = val < SHRT_MIN ? SHRT_MIN : val > SHRT_MAX ? SHRT_MAX : val;
    }
    // Free the temporary buffer we made to handle overflows
	delete[] overflowBuff;
}


void* AudioMixer::playbackThread(void* arg)
{
    (void)arg;
	while (!stopping) {
		// Generate next block of audio
		fillPlaybackBuffer(playbackBuffer, playbackBufferSize);

		// Output the audio
		snd_pcm_sframes_t frames = snd_pcm_writei(handle,
				playbackBuffer, playbackBufferSize);

		// Check for (and handle) possible error conditions on output
		if (frames < 0) {
			std::cerr << "AudioMixer: writei() returned " << frames << std::endl;
			frames = snd_pcm_recover(handle, frames, 1);
		}
		if (frames < 0) {
			std::cerr << "ERROR: Failed writing audio with snd_pcm_writei(): " 
				<< frames << std::endl;
			exit(EXIT_FAILURE);
		}
		if (frames > 0 && (unsigned long)frames < playbackBufferSize) {
			std::cerr << "Short write (expected " << playbackBufferSize
				<< ", wrote " << frames << ")" << std::endl;
		}
	}

	return NULL;
}