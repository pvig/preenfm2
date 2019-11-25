/*
 * Copyright 2013 Xavier Hosxe
 *
 * Author: Xavier Hosxe (xavier <.> hosxe < a t > gmail.com)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <math.h>
#include "Timbre.h"
#include "Voice.h"

#define INV127 .00787401574803149606f
#define INV16 .0625f
#define filterWindowMin 0.01f
#define filterWindowMax 0.99f
// Regular memory
float midiNoteScale[2][NUMBER_OF_TIMBRES][128];

/*
#include "LiquidCrystal.h"
extern LiquidCrystal lcd;
void myVoiceError(char info, int t, int t2) {
    lcd.setRealTimeAction(true);
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print('!');
    lcd.print(info);
    lcd.print(t);
    lcd.print(' ');
    lcd.print(t2);
    while (true) {};
}

...
        if (voiceNumber[k] < 0) myVoiceError('A', voiceNumber[k], k);

*/

//#define DEBUG_ARP_STEP
enum ArpeggiatorDirection {
    ARPEGGIO_DIRECTION_UP = 0,
    ARPEGGIO_DIRECTION_DOWN,
    ARPEGGIO_DIRECTION_UP_DOWN,
    ARPEGGIO_DIRECTION_PLAYED,
    ARPEGGIO_DIRECTION_RANDOM,
    ARPEGGIO_DIRECTION_CHORD,
   /*
    * ROTATE modes rotate the first note played, e.g. UP: C-E-G -> E-G-C -> G-C-E -> repeat
    */
    ARPEGGIO_DIRECTION_ROTATE_UP, ARPEGGIO_DIRECTION_ROTATE_DOWN, ARPEGGIO_DIRECTION_ROTATE_UP_DOWN,
   /*
    * SHIFT modes rotate and extend with transpose, e.g. UP: C-E-G -> E-G-C1 -> G-C1-E1 -> repeat
    */
    ARPEGGIO_DIRECTION_SHIFT_UP, ARPEGGIO_DIRECTION_SHIFT_DOWN, ARPEGGIO_DIRECTION_SHIFT_UP_DOWN,

    ARPEGGIO_DIRECTION_COUNT
};

// TODO Maybe add something like struct ArpDirectionParams { dir, can_change, use_start_step }

inline static int __getDirection( int _direction ) {
	switch( _direction ) {
	case ARPEGGIO_DIRECTION_DOWN:
	case ARPEGGIO_DIRECTION_ROTATE_DOWN:
	case ARPEGGIO_DIRECTION_SHIFT_DOWN:
		return -1;
	default:
		return 1;
	}
}

inline static int __canChangeDir( int _direction ) {
	switch( _direction ) {
	case ARPEGGIO_DIRECTION_UP_DOWN:
	case ARPEGGIO_DIRECTION_ROTATE_UP_DOWN:
	case ARPEGGIO_DIRECTION_SHIFT_UP_DOWN:
		return 1;
	default:
		return 0;
	}
}

inline static int __canTranspose( int _direction ) {
	switch( _direction ) {
	case ARPEGGIO_DIRECTION_SHIFT_UP:
	case ARPEGGIO_DIRECTION_SHIFT_DOWN:
	case ARPEGGIO_DIRECTION_SHIFT_UP_DOWN:
		return 1;
	default:
		return 0;
	}
}

//for bitwise manipulations
#define FLOAT2SHORT 32768.f
#define SHORT2FLOAT 1./32768.f

#define RATIOINV 1./131072.f

#define SVFRANGE 1.23f
#define SVFOFFSET 0.151f
#define SVFGAINOFFSET 0.3f

#define LP2OFFSET -0.045f
#define min(a,b)                ((a)<(b)?(a):(b))

extern float noise[32];

inline 
float expf_fast(float a) {
  //https://github.com/ekmett/approximate/blob/master/cbits/fast.c
  union { float f; int x; } u;
  u.x = (int) (12102203 * a + 1064866805);
  return u.f;
}
inline
float sqrt3(const float x)  
{
  union
  {
    int i;
    float x;
  } u;

  u.x = x;
  u.i = (1 << 29) + (u.i >> 1) - (1 << 22);
  return u.x;
} 
inline
float tanh3(float x)
{
	//return x / (0.7f + fabsf(0.8f * x));
	return 1.5f * x / (1.7f + fabsf(0.34f * x * x));
}
inline
float tanh4(float x)
{
	return x / sqrt3(x * x + 1);
}
inline
float sat25(float x)
{ 
	if (unlikely(fabsf(x) > 4))
		return 0;
	return x * (1.f - fabsf(x * 0.25f));
}
inline
float sat33(float x)
{
	if (unlikely(fabsf(x) > 2.58f))
		return 0;
	return x * (1 - x * x * 0.15f);
}
inline
float clamp(float d, float min, float max) {
  const float t = unlikely(d < min) ? min : d;
  return unlikely(t > max) ? max : t;
}
inline 
float satSin(float x, float drive, uint_fast16_t pos) {
	//https://www.desmos.com/calculator/pdsqpi5lp6
	return x + clamp(x, -0.5f * drive, drive) * sinTable[((uint_fast16_t)(fabsf(x) * 360) + pos) & 0x3FF];
}
//https://www.musicdsp.org/en/latest/Other/120-saturation.html
inline
float sigmoid(float x)
{
	return x * (1.5f - 0.5f * x * x);
}
inline
float sigmoidPos(float x)
{
	//x : 0 -> 1
	return (sigmoid((x * 2) - 1) + 1) * 0.5f;
}
float fold(float x4) {
	// https://www.desmos.com/calculator/ge2wvg2wgj
	// x4 = x / 4
	return (fabsf(x4 + 0.25f - roundf(x4 + 0.25f)) - 0.25f);
}
inline
float wrap(float x) {
	return x - floorf(x);
}
inline 
float window(float x) {
	if (x < 0 || x > 1) {
		return 0;
	} else if (x < 0.2f) {
		return sqrt3( x * 5 );
	} else if (x > 0.8f) {
		return sqrt3(1 - (x-0.8f) * 5);
	} else {
		return 1;
	}
}
inline float ftan(float fAngle)
{
	float fASqr = fAngle * fAngle;
	float fResult = 2.033e-01f;
	fResult *= fASqr;
	fResult += 3.1755e-01f;
	fResult *= fASqr;
	fResult += 1.0f;
	fResult *= fAngle;
	return fResult;
}
enum NewNoteType {
	NEW_NOTE_FREE = 0,
	NEW_NOTE_RELEASE,
	NEW_NOTE_OLD,
	NEW_NOTE_NONE
};


arp_pattern_t lut_res_arpeggiator_patterns[ ARPEGGIATOR_PRESET_PATTERN_COUNT ]  = {
  ARP_PATTERN(21845), ARP_PATTERN(62965), ARP_PATTERN(46517), ARP_PATTERN(54741),
  ARP_PATTERN(43861), ARP_PATTERN(22869), ARP_PATTERN(38293), ARP_PATTERN(2313),
  ARP_PATTERN(37449), ARP_PATTERN(21065), ARP_PATTERN(18761), ARP_PATTERN(54553),
  ARP_PATTERN(27499), ARP_PATTERN(23387), ARP_PATTERN(30583), ARP_PATTERN(28087),
  ARP_PATTERN(22359), ARP_PATTERN(28527), ARP_PATTERN(30431), ARP_PATTERN(43281),
  ARP_PATTERN(28609), ARP_PATTERN(53505)
};

uint16_t Timbre::getArpeggiatorPattern() const
{
  const int pattern = (int)params.engineArp2.pattern;
  if ( pattern < ARPEGGIATOR_PRESET_PATTERN_COUNT )
    return ARP_PATTERN_GETMASK(lut_res_arpeggiator_patterns[ pattern ]);
  else
    return ARP_PATTERN_GETMASK( params.engineArpUserPatterns.patterns[ pattern - ARPEGGIATOR_PRESET_PATTERN_COUNT ] );
}

const uint8_t midi_clock_tick_per_step[17]  = {
  192, 144, 96, 72, 64, 48, 36, 32, 24, 16, 12, 8, 6, 4, 3, 2, 1
};

float panTable[] = {
		0.0000, 0.0007, 0.0020, 0.0036, 0.0055, 0.0077, 0.0101, 0.0128, 0.0156, 0.0186,
		0.0218, 0.0252, 0.0287, 0.0324, 0.0362, 0.0401, 0.0442, 0.0484, 0.0527, 0.0572,
		0.0618, 0.0665, 0.0713, 0.0762, 0.0812, 0.0863, 0.0915, 0.0969, 0.1023, 0.1078,
		0.1135, 0.1192, 0.1250, 0.1309, 0.1369, 0.1430, 0.1492, 0.1554, 0.1618, 0.1682,
		0.1747, 0.1813, 0.1880, 0.1947, 0.2015, 0.2085, 0.2154, 0.2225, 0.2296, 0.2369,
		0.2441, 0.2515, 0.2589, 0.2664, 0.2740, 0.2817, 0.2894, 0.2972, 0.3050, 0.3129,
		0.3209, 0.3290, 0.3371, 0.3453, 0.3536, 0.3619, 0.3703, 0.3787, 0.3872, 0.3958,
		0.4044, 0.4131, 0.4219, 0.4307, 0.4396, 0.4485, 0.4575, 0.4666, 0.4757, 0.4849,
		0.4941, 0.5034, 0.5128, 0.5222, 0.5316, 0.5411, 0.5507, 0.5604, 0.5700, 0.5798,
		0.5896, 0.5994, 0.6093, 0.6193, 0.6293, 0.6394, 0.6495, 0.6597, 0.6699, 0.6802,
		0.6905, 0.7009, 0.7114, 0.7218, 0.7324, 0.7430, 0.7536, 0.7643, 0.7750, 0.7858,
		0.7967, 0.8076, 0.8185, 0.8295, 0.8405, 0.8516, 0.8627, 0.8739, 0.8851, 0.8964,
		0.9077, 0.9191, 0.9305, 0.9420, 0.9535, 0.9651, 0.9767, 0.9883, 1.0000, 1.0000,
		1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000,
		1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000,
		1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000,
		1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000,
		1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000,
		1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000,
		1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000,
		1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000,
		1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000,
		1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000,
		1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000,
		1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000,
		1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000
} ;



// Static to all 4 timbres
unsigned int voiceIndex  __attribute__ ((section(".ccmnoload")));

Timbre::Timbre() {


    this->recomputeNext = true;
	this->currentGate = 0;
    this->sbMax = &this->sampleBlock[64];
    this->holdPedal = false;
    this->lastPlayedNote = 0;
    // arpegiator
    setNewBPMValue(90);
    arpegiatorStep = 0.0;
    idle_ticks_ = 96;
    running_ = 0;
    ignore_note_off_messages_ = 0;
    recording_ = 0;
    note_stack.Init();
    event_scheduler.Init();
    // Arpeggiator start
    Start();


    // Init FX variables
	v0L = v1L = v2L = v3L = v4L = v5L = v6L = v7L = v8L = v0R = v1R = v2R = v3R = v4R = v5R = v6R = v7R = v8R = v8R = 0.0f;
    fxParam1PlusMatrix = -1.0;
}

Timbre::~Timbre() {
}

void Timbre::init(int timbreNumber) {


	env1.init(&params.env1a,  &params.env1b, 0, &params.engine1.algo);
	env2.init(&params.env2a,  &params.env2b, 1, &params.engine1.algo);
	env3.init(&params.env3a,  &params.env3b, 2, &params.engine1.algo);
	env4.init(&params.env4a,  &params.env4b, 3, &params.engine1.algo);
	env5.init(&params.env5a,  &params.env5b, 4, &params.engine1.algo);
	env6.init(&params.env6a,  &params.env6b, 5, &params.engine1.algo);

	osc1.init(&params.osc1, OSC1_FREQ);
	osc2.init(&params.osc2, OSC2_FREQ);
	osc3.init(&params.osc3, OSC3_FREQ);
	osc4.init(&params.osc4, OSC4_FREQ);
	osc5.init(&params.osc5, OSC5_FREQ);
	osc6.init(&params.osc6, OSC6_FREQ);

    this->timbreNumber = timbreNumber;

    for (int s=0; s<2; s++) {
        for (int n=0; n<128; n++) {
            midiNoteScale[s][timbreNumber][n] = INV127 * (float)n;
        }
    }
    for (int lfo=0; lfo<NUMBER_OF_LFO; lfo++) {
        lfoUSed[lfo] = 0;
    }

}

void Timbre::setVoiceNumber(int v, int n) {
	voiceNumber[v] = n;
	if (n >=0) {
		voices[n]->setCurrentTimbre(this);
	}
}


void Timbre::initVoicePointer(int n, Voice* voice) {
	voices[n] = voice;
}

void Timbre::noteOn(uint8_t note, uint8_t velocity) {
	if (params.engineArp1.clock) {
		arpeggiatorNoteOn(note, velocity);
	} else {
		preenNoteOn(note, velocity);
	}
}

void Timbre::noteOff(uint8_t note) {
	if (params.engineArp1.clock) {
		arpeggiatorNoteOff(note);
	} else {
		preenNoteOff(note);
	}
}

int cptHighNote = 0;

void Timbre::preenNoteOn(uint8_t note, uint8_t velocity) {

	int iNov = (int) params.engine1.numberOfVoice;
	if (unlikely(iNov == 0)) {
		return;
	}

	unsigned int indexMin = (unsigned int)2147483647;
	int voiceToUse = -1;

	int newNoteType = NEW_NOTE_NONE;

	for (int k = 0; k < iNov; k++) {
		// voice number k of timbre
		int n = voiceNumber[k];

        if (unlikely(voices[n]->isNewNotePending())) {
            continue;
        }

		// same note = priority 1 : take the voice immediatly
		if (unlikely(voices[n]->isPlaying() && voices[n]->getNote() == note)) {

#ifdef DEBUG_VOICE
		lcd.setRealTimeAction(true);
		lcd.setCursor(16,1);
		lcd.print(cptHighNote++);
		lcd.setCursor(16,2);
		lcd.print("S:");
		lcd.print(n);
#endif

            preenNoteOnUpdateMatrix(n, note, velocity);
            voices[n]->noteOnWithoutPop(note, velocity, voiceIndex++);
            this->lastPlayedNote = n;
			return;
		}

		// unlikely because if it true, CPU is not full
		if (unlikely(newNoteType > NEW_NOTE_FREE)) {
			if (!voices[n]->isPlaying()) {
				voiceToUse = n;
				newNoteType = NEW_NOTE_FREE;
			}

			if (voices[n]->isReleased()) {
				int indexVoice = voices[n]->getIndex();
				if (indexVoice < indexMin) {
					indexMin = indexVoice;
					voiceToUse = n;
					newNoteType = NEW_NOTE_RELEASE;
				}
			}
		}
	}

	if (voiceToUse == -1) {
		for (int k = 0; k < iNov; k++) {
			// voice number k of timbre
			int n = voiceNumber[k];
			int indexVoice = voices[n]->getIndex();
			if (indexVoice < indexMin && !voices[n]->isNewNotePending()) {
				newNoteType = NEW_NOTE_OLD;
				indexMin = indexVoice;
				voiceToUse = n;
			}
		}
	}
	// All voices in newnotepending state ?
	if (voiceToUse != -1) {
#ifdef DEBUG_VOICE
		lcd.setRealTimeAction(true);
		lcd.setCursor(16,1);
		lcd.print(cptHighNote++);
		lcd.setCursor(16,2);
		switch (newNoteType) {
			case NEW_NOTE_FREE:
				lcd.print("F:");
				break;
			case NEW_NOTE_OLD:
				lcd.print("O:");
				break;
			case NEW_NOTE_RELEASE:
				lcd.print("R:");
				break;
		}
		lcd.print(voiceToUse);
#endif


		preenNoteOnUpdateMatrix(voiceToUse, note, velocity);

		switch (newNoteType) {
		case NEW_NOTE_FREE:
			voices[voiceToUse]->noteOn(note, velocity, voiceIndex++);
			break;
		case NEW_NOTE_OLD:
		case NEW_NOTE_RELEASE:
			voices[voiceToUse]->noteOnWithoutPop(note, velocity, voiceIndex++);
			break;
		}

		this->lastPlayedNote = voiceToUse;
	}
}

void Timbre::preenNoteOnUpdateMatrix(int voiceToUse, int note, int velocity) {
    // Update voice matrix with midi note and velocity
    if (likely(note < 128)) {
        voices[voiceToUse]->matrix.setSource(MATRIX_SOURCE_NOTE1, midiNoteScale[0][timbreNumber][note]);
        voices[voiceToUse]->matrix.setSource(MATRIX_SOURCE_NOTE2, midiNoteScale[1][timbreNumber][note]);
        voices[voiceToUse]->matrix.setSource(MATRIX_SOURCE_VELOCITY, INV127*velocity);
    }
}


void Timbre::propagateCvFreq(uint8_t note) {
    int iNov = (int) params.engine1.numberOfVoice;
    for (int k = 0; k < iNov; k++) {
        int n = voiceNumber[k];
        if (voices[n]->getNote() == note) {
            if (voices[n]->isPlaying()) {
                voices[n]->propagateCvFreq(note);
            }
            return;
        }
    }
}

void Timbre::preenNoteOff(uint8_t note) {
	int iNov = (int) params.engine1.numberOfVoice;
	for (int k = 0; k < iNov; k++) {
		// voice number k of timbre
		int n = voiceNumber[k];

		// Not playing = free CPU
		if (unlikely(!voices[n]->isPlaying())) {
			continue;
		}

		if (likely(voices[n]->getNextGlidingNote() == 0)) {
			if (voices[n]->getNote() == note) {
				if (unlikely(holdPedal)) {
					voices[n]->setHoldedByPedal(true);
					return;
				} else {
					voices[n]->noteOff();
					return;
				}
			}
		} else {
			// if gliding and releasing first note
			if (voices[n]->getNote() == note) {
				voices[n]->glideFirstNoteOff();
				return;
			}
			// if gliding and releasing next note
			if (voices[n]->getNextGlidingNote() == note) {
				voices[n]->glideToNote(voices[n]->getNote());
				voices[n]->glideFirstNoteOff();
				return;
			}
		}
	}
}


void Timbre::setHoldPedal(int value) {
	if (value <64) {
		holdPedal = false;
	    int numberOfVoices = params.engine1.numberOfVoice;
	    for (int k = 0; k < numberOfVoices; k++) {
	        // voice number k of timbre
	        int n = voiceNumber[k];
	        if (voices[n]->isHoldedByPedal()) {
	        	voices[n]->noteOff();
	        }
	    }
	    arpeggiatorSetHoldPedal(0);
	} else {
		holdPedal = true;
	    arpeggiatorSetHoldPedal(127);
	}
}




void Timbre::setNewBPMValue(float bpm) {
	ticksPerSecond = bpm * 24.0f / 60.0f;
	ticksEveryNCalls = calledPerSecond / ticksPerSecond;
	ticksEveyNCallsInteger = (int)ticksEveryNCalls;
}

void Timbre::setArpeggiatorClock(float clockValue) {
	if (clockValue == CLOCK_OFF) {
		FlushQueue();
		note_stack.Clear();
	}
	if (clockValue == CLOCK_INTERNAL) {
	    setNewBPMValue(params.engineArp1.BPM);
	}
	if (clockValue == CLOCK_EXTERNAL) {
		// Let's consider we're running
		running_ = 1;
	}
}


void Timbre::prepareForNextBlock() {

	// Apeggiator clock : internal
	if (params.engineArp1.clock == CLOCK_INTERNAL) {
		arpegiatorStep+=1.0f;
		if (unlikely((arpegiatorStep) > ticksEveryNCalls)) {
			arpegiatorStep -= ticksEveyNCallsInteger;
			Tick();
		}
	}
}

void Timbre::cleanNextBlock() {

	float *sp = this->sampleBlock;
	while (sp < this->sbMax) {
		*sp++ = 0;
		*sp++ = 0;
		*sp++ = 0;
		*sp++ = 0;
		*sp++ = 0;
		*sp++ = 0;
		*sp++ = 0;
		*sp++ = 0;
	}
}


void Timbre::prepareMatrixForNewBlock() {
    for (int k = 0; k < params.engine1.numberOfVoice; k++) {
        voices[voiceNumber[k]]->prepareMatrixForNewBlock();
    }
}


#define GATE_INC 0.02f

void Timbre::fxAfterBlock(float ratioTimbres) {

    // Gate algo !!
    float gate = voices[this->lastPlayedNote]->matrix.getDestination(MAIN_GATE);
    if (unlikely(gate > 0 || currentGate > 0)) {
		gate *=.72547132656922730694f; // 0 < gate < 1.0
		if (gate > 1.0f) {
			gate = 1.0f;
		}
		float incGate = (gate - currentGate) * .03125f; // ( *.03125f = / 32)
		// limit the speed.
		if (incGate > 0.002f) {
			incGate = 0.002f;
		} else if (incGate < -0.002f) {
			incGate = -0.002f;
		}

		float *sp = this->sampleBlock;
		float coef;
    	for (int k=0 ; k< BLOCK_SIZE ; k++) {
			currentGate += incGate;
			coef = 1.0f - currentGate;
			*sp = *sp * coef;
			sp++;
			*sp = *sp * coef;
			sp++;
		}
    //    currentGate = gate;
    }

    float matrixFilterFrequency = voices[this->lastPlayedNote]->matrix.getDestination(FILTER_FREQUENCY);
	float numberVoicesAttn = 1 - (params.engine1.numberOfVoice * 0.04f * ratioTimbres * RATIOINV);

    // LP Algo
    int effectType = params.effect.type;
    float gainTmp =  params.effect.param3 * numberOfVoiceInverse * ratioTimbres;
    mixerGain = 0.02f * gainTmp + .98f  * mixerGain;

    switch (effectType) {
    case FILTER_LP:
    {
    	float fxParamTmp = params.effect.param1 + matrixFilterFrequency;
    	fxParamTmp *= fxParamTmp;

    	// Low pass... on the Frequency
    	fxParam1 = (fxParamTmp + 9.0f * fxParam1) * .1f;
    	if (unlikely(fxParam1 > 1.0f)) {
    		fxParam1 = 1.0f;
    	}
    	if (unlikely(fxParam1 < 0.0f)) {
    		fxParam1 = 0.0f;
    	}

    	float pattern = (1 - fxParam2 * fxParam1);

    	float *sp = this->sampleBlock;
    	float localv0L = v0L;
    	float localv1L = v1L;
    	float localv0R = v0R;
    	float localv1R = v1R;

    	for (int k=BLOCK_SIZE ; k--; ) {

    		// Left voice
    		localv0L =  pattern * localv0L  -  (fxParam1) * (localv1L + *sp);
    		localv1L =  pattern * localv1L  +  (fxParam1) * localv0L;

    		*sp = localv1L * mixerGain;

    		if (unlikely(*sp > ratioTimbres)) {
    			*sp = ratioTimbres;
    		}
    		if (unlikely(*sp < -ratioTimbres)) {
    			*sp = -ratioTimbres;
    		}

    		sp++;

    		// Right voice
    		localv0R =  pattern * localv0R  -  (fxParam1) * (localv1R + *sp);
    		localv1R =  pattern * localv1R  +  (fxParam1) * localv0R;

    		*sp = localv1R * mixerGain;

    		if (unlikely(*sp > ratioTimbres)) {
    			*sp = ratioTimbres;
    		}
    		if (unlikely(*sp < -ratioTimbres)) {
    			*sp = -ratioTimbres;
    		}

    		sp++;
    	}
    	v0L = localv0L;
    	v1L = localv1L;
    	v0R = localv0R;
    	v1R = localv1R;
    }
    break;
    case FILTER_HP:
    {
    	float fxParamTmp = params.effect.param1 + matrixFilterFrequency;
    	fxParamTmp *= fxParamTmp;

    	// Low pass... on the Frequency
    	fxParam1 = (fxParamTmp + 9.0f * fxParam1) * .1f;
    	if (unlikely(fxParam1 > 1.0)) {
    		fxParam1 = 1.0;
    	}
        if (unlikely(fxParam1 < 0.0f)) {
            fxParam1 = 0.0f;
        }
    	float pattern = (1 - fxParam2 * fxParam1);

    	float *sp = this->sampleBlock;
    	float localv0L = v0L;
    	float localv1L = v1L;
    	float localv0R = v0R;
    	float localv1R = v1R;

    	for (int k=0 ; k < BLOCK_SIZE ; k++) {

    		// Left voice
    		localv0L =  pattern * localv0L  -  (fxParam1) * localv1L  + (fxParam1) * (*sp);
    		localv1L =  pattern * localv1L  +  (fxParam1) * localv0L;

    		*sp = (*sp - localv1L) * mixerGain;

    		if (unlikely(*sp > ratioTimbres)) {
    			*sp = ratioTimbres;
    		}
    		if (unlikely(*sp < -ratioTimbres)) {
    			*sp = -ratioTimbres;
    		}

    		sp++;

    		// Right voice
    		localv0R =  pattern * localv0R  -  (fxParam1) * localv1R  + (fxParam1) * (*sp);
    		localv1R =  pattern * localv1R  +  (fxParam1) * localv0R;

    		*sp = (*sp - localv1R) * mixerGain;

    		if (unlikely(*sp > ratioTimbres)) {
    			*sp = ratioTimbres;
    		}
    		if (unlikely(*sp < -ratioTimbres)) {
    			*sp = -ratioTimbres;
    		}

    		sp++;
    	}
    	v0L = localv0L;
    	v1L = localv1L;
    	v0R = localv0R;
    	v1R = localv1R;

    }
	break;
    case FILTER_BASS:
    {
    	// From musicdsp.com
    	//    	Bass Booster
    	//
    	//    	Type : LP and SUM
    	//    	References : Posted by Johny Dupej
    	//
    	//    	Notes :
    	//    	This function adds a low-passed signal to the original signal. The low-pass has a quite wide response.
    	//
    	//    	selectivity - frequency response of the LP (higher value gives a steeper one) [70.0 to 140.0 sounds good]
    	//    	ratio - how much of the filtered signal is mixed to the original
    	//    	gain2 - adjusts the final volume to handle cut-offs (might be good to set dynamically)

    	//static float selectivity, gain1, gain2, ratio, cap;
    	//gain1 = 1.0/(selectivity + 1.0);
    	//
    	//cap= (sample + cap*selectivity )*gain1;
    	//sample = saturate((sample + cap*ratio)*gain2);

    	float *sp = this->sampleBlock;
    	float localv0L = v0L;
    	float localv0R = v0R;

    	for (int k=0 ; k < BLOCK_SIZE ; k++) {

    		localv0L = ((*sp) + localv0L * fxParam1) * fxParam3;
    		(*sp) = ((*sp) + localv0L * fxParam2) * mixerGain;

    		if (unlikely(*sp > ratioTimbres)) {
    			*sp = ratioTimbres;
    		}
    		if (unlikely(*sp < -ratioTimbres)) {
    			*sp = -ratioTimbres;
    		}

    		sp++;

    		localv0R = ((*sp) + localv0R * fxParam1) * fxParam3;
    		(*sp) = ((*sp) + localv0R * fxParam2) * mixerGain;

    		if (unlikely(*sp > ratioTimbres)) {
    			*sp = ratioTimbres;
    		}
    		if (unlikely(*sp < -ratioTimbres)) {
    			*sp = -ratioTimbres;
    		}

    		sp++;
    	}
    	v0L = localv0L;
    	v0R = localv0R;

    }
    break;
    case FILTER_MIXER:
    {
		float fxParamTmp = params.effect.param1 + matrixFilterFrequency;
		// Low pass... on the Frequency
		fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

    	float pan = fxParam1 * 2 - 1.0f ;
    	float *sp = this->sampleBlock;
    	float sampleR, sampleL;
    	if (pan <= 0) {
        	float onePlusPan = 1 + pan;
        	float minusPan = - pan;
        	for (int k=BLOCK_SIZE ; k--; ) {
				sampleL = *(sp);
				sampleR = *(sp + 1);

				*sp = (sampleL + sampleR * minusPan) * mixerGain;
				sp++;
				*sp = sampleR * onePlusPan * mixerGain;
				sp++;
			}
    	} else if (pan > 0) {
        	float oneMinusPan = 1 - pan;
        	float adjustedmixerGain = (pan * .5) * mixerGain;
        	for (int k=0 ; k < BLOCK_SIZE ; k++) {
				sampleL = *(sp);
				sampleR = *(sp + 1);

				*sp = sampleL * oneMinusPan * mixerGain;
				sp++;
				*sp = (sampleR + sampleL * pan) * mixerGain;
				sp++;
			}
    	}
    }
    break;
    case FILTER_CRUSHER:
    {
        // Algo from http://www.musicdsp.org/archive.php?classid=4#139
        // Lo-Fi Crusher
        // Type : Quantizer / Decimator with smooth control
        // References : Posted by David Lowenfels

        //        function output = crusher( input, normfreq, bits );
        //            step = 1/2^(bits);
        //            phasor = 0;
        //            last = 0;
        //
        //            for i = 1:length(input)
        //               phasor = phasor + normfreq;
        //               if (phasor >= 1.0)
        //                  phasor = phasor - 1.0;
        //                  last = step * floor( input(i)/step + 0.5 ); %quantize
        //               end
        //               output(i) = last; %sample and hold
        //            end
        //        end


        float fxParamTmp = params.effect.param1 + matrixFilterFrequency +.005f;
        if (unlikely(fxParamTmp > 1.0)) {
            fxParamTmp = 1.0;
        }
        if (unlikely(fxParamTmp < 0.005f)) {
            fxParamTmp = 0.005f;
        }
        fxParamA1 = (fxParamTmp + 9.0f * fxParamA1) * .1f;
        // Low pass... on the Sampling rate
        register float fxFreq = fxParamA1;

        register float *sp = this->sampleBlock;

        register float localPhase = v7R;

        //        localPower = fxParam1 = pow(2, (int)(1.0f + 15.0f * params.effect.param2));
        //        localStep = fxParam2 = 1 / fxParam1;

        register float localPower = fxParam1;
        register float localStep = fxParam2;

        register float localv0L = v0L;
        register float localv0R = v0R;


        for (int k=0 ; k < BLOCK_SIZE ; k++) {
            localPhase += fxFreq;
            if (unlikely(localPhase >= 1.0f)) {
                localPhase -= 1.0f;
                // Simulate floor by making the conversion always positive
                // simplify version
                register int iL =  (*sp) * localPower + .75f;
                register int iR =  (*(sp + 1)) * localPower + .75f;
                localv0L = localStep * iL;
                localv0R = localStep * iR;
            }

            *sp++ = localv0L * mixerGain;
            *sp++ = localv0R * mixerGain;
        }
        v0L = localv0L;
        v0R = localv0R;
        v7R = localPhase;

    }
    break;
    case FILTER_BP:
    {
//        float input;                    // input sample
//        float output;                   // output sample
//        float v;                        // This is the intermediate value that
//                                        //    gets stored in the delay registers
//        float old1;                     // delay register 1, initialized to 0
//        float old2;                     // delay register 2, initialized to 0
//
//        /* filter coefficients */
//        omega1  = 2 * PI * f/srate; // f is your center frequency
//        sn1 = (float)sin(omega1);
//        cs1 = (float)cos(omega1);
//        alpha1 = sn1/(2*Qvalue);        // Qvalue is none other than Q!
//        a0 = 1.0f + alpha1;     // a0
//        b0 = alpha1;            // b0
//        b1 = 0.0f;          // b1/b0
//        b2= -alpha1/b0          // b2/b0
//        a1= -2.0f * cs1/a0;     // a1/a0
//        a2= (1.0f - alpha1)/a0;          // a2/a0
//        k = b0/a0;
//
//        /* The filter code */
//
//        v = k*input - a1*old1 - a2*old2;
//        output = v + b1*old1 + b2*old2;
//        old2 = old1;
//        old1 = v;

        // fxParam1 v
        //

        float fxParam1PlusMatrixTmp = params.effect.param1 + matrixFilterFrequency;
        if (unlikely(fxParam1PlusMatrixTmp > 1.0f)) {
            fxParam1PlusMatrixTmp = 1.0f;
        }
        if (unlikely(fxParam1PlusMatrixTmp < 0.0f)) {
            fxParam1PlusMatrixTmp = 0.0f;
        }

		if (fxParam1PlusMatrix != fxParam1PlusMatrixTmp) {
			fxParam1PlusMatrix = fxParam1PlusMatrixTmp;
			recomputeBPValues(params.effect.param2, fxParam1PlusMatrix * fxParam1PlusMatrix);
		}

        float localv0L = v0L;
        float localv0R = v0R;
        float localv1L = v1L;
        float localv1R = v1R;
        float *sp = this->sampleBlock;

        for (int k=0 ; k < BLOCK_SIZE ; k++) {
            float localV = fxParam1 /* k */ * (*sp) - fxParamA1 * localv0L - fxParamA2 * localv1L;
            *sp++ = (localV + /* fxParamB1 (==0) * localv0L  + */ fxParamB2 * localv1L) * mixerGain;
            if (unlikely(*sp > ratioTimbres)) {
                *sp = ratioTimbres;
            }
            if (unlikely(*sp < -ratioTimbres)) {
                *sp = -ratioTimbres;
            }
            localv1L = localv0L;
            localv0L = localV;

            localV = fxParam1 /* k */ * (*sp) - fxParamA1 * localv0R - fxParamA2 * localv1R;
            *sp++ = (localV + /* fxParamB1 (==0) * localv0R + */ fxParamB2 * localv1R) * mixerGain;
            if (unlikely(*sp > ratioTimbres)) {
                *sp = ratioTimbres;
            }
            if (unlikely(*sp < -ratioTimbres)) {
                *sp = -ratioTimbres;
            }
            localv1R = localv0R;
            localv0R = localV;


        }

        v0L = localv0L;
        v0R = localv0R;
        v1L = localv1L;
        v1R = localv1R;

        break;
    }
case FILTER_LP2:
    {
		float fxParamTmp = LP2OFFSET + params.effect.param1 + matrixFilterFrequency;
		fxParamTmp *= fxParamTmp;

		// Low pass... on the Frequency
		fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

		const float f = (fxParam1);
    	const float pattern = (1 - fxParam2 * f * 0.997f);

    	float *sp = this->sampleBlock;
    	float localv0L = v0L;
    	float localv1L = v1L;
    	float localv0R = v0R;
    	float localv1R = v1R;

		float _ly1L = v2L, _ly1R = v2R;
		float _lx1L = v3L, _lx1R = v3R;
		const float f1 = clamp(0.27f + f * 0.33f, filterWindowMin, filterWindowMax);
		float coef1 = (1.0f - f1) / (1.0f + f1);

    	for (int k=BLOCK_SIZE ; k--; ) {

			// Left voice
			localv0L = pattern * localv0L - f * sat25(localv1L + *sp);
			localv1L = pattern * localv1L + f * localv0L;

			localv0L = pattern * localv0L - f * (localv1L + *sp);
			localv1L = pattern * localv1L + f * localv0L;

			_ly1L = coef1 * (_ly1L + localv1L) - _lx1L; // allpass
			_lx1L = localv1L;

			*sp++ = clamp(_ly1L * mixerGain, -ratioTimbres, ratioTimbres);

			// Right voice
			localv0R = pattern * localv0R - f * sat25(localv1R + *sp);
			localv1R = pattern * localv1R + f * localv0R;

			localv0R = pattern * localv0R - f * (localv1R + *sp);
			localv1R = pattern * localv1R + f * localv0R;

			_ly1R = coef1 * (_ly1R + localv1R) - _lx1R; // allpass
			_lx1R = localv1R;

			*sp++ = clamp(_ly1R * mixerGain, -ratioTimbres, ratioTimbres);
		}
    	v0L = localv0L;
    	v1L = localv1L;
    	v0R = localv0R;
    	v1R = localv1R;

		v2L = _ly1L; v2R = _ly1R;
		v3L = _lx1L; v3R = _lx1R;
    }
    break;
case FILTER_HP2:
    {
		float fxParamTmp = LP2OFFSET + params.effect.param1 + matrixFilterFrequency;
		fxParamTmp *= fxParamTmp;

		// Low pass... on the Frequency
		fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

		const float f = (fxParam1);
        const float pattern = (1 - fxParam2 * f);

        float *sp = this->sampleBlock;
        float localv0L = v0L;
        float localv1L = v1L;
        float localv0R = v0R;
        float localv1R = v1R;

        for (int k=0 ; k < BLOCK_SIZE ; k++) {

			// Left voice
			localv0L = pattern * localv0L + f * sat33(-localv1L + *sp);
			localv1L = pattern * localv1L + f * localv0L;

			localv0L = pattern * localv0L + f * (-localv1L + *sp);
			localv1L = pattern * localv1L + f * localv0L;

			*sp++ = clamp((*sp - localv1L) * mixerGain, -ratioTimbres, ratioTimbres);

			// Right voice
			localv0R = pattern * localv0R + f * sat33(-localv1R + *sp);
			localv1R = pattern * localv1R + f * localv0R;

			localv0R = pattern * localv0R + f * (-localv1R + *sp);
			localv1R = pattern * localv1R + f * localv0R;

			*sp++ = clamp((*sp - localv1R) * mixerGain, -ratioTimbres, ratioTimbres);
		}
        v0L = localv0L;
        v1L = localv1L;
        v0R = localv0R;
        v1R = localv1R;

    }
    break;
case FILTER_BP2:
{
    float fxParam1PlusMatrixTmp = clamp(params.effect.param1 + matrixFilterFrequency, 0, 1);
    if (fxParam1PlusMatrix != fxParam1PlusMatrixTmp) {
        fxParam1PlusMatrix = fxParam1PlusMatrixTmp;
        recomputeBPValues(params.effect.param2, fxParam1PlusMatrix * fxParam1PlusMatrix);
    }

    float localv0L = v0L;
    float localv0R = v0R;
    float localv1L = v1L;
    float localv1R = v1R;
    float *sp = this->sampleBlock;
    float in,temp,localV;

    for (int k=0 ; k < BLOCK_SIZE ; k++) {
		//Left
		in = fxParam1 * sat33(*sp);
		temp = in - fxParamA1 * localv0L - fxParamA2 * localv1L;
		localv1L = localv0L;
		localv0L = temp;
		localV = (temp + (in - fxParamA1 * localv0L - fxParamA2 * localv1L)) * 0.5f;
		*sp++ = clamp((localV + fxParamB2 * localv1L) * mixerGain, -ratioTimbres, ratioTimbres);

		localv1L = localv0L;
		localv0L = localV;

		//Right
		in = fxParam1 * sat33(*sp);
		temp = in - fxParamA1 * localv0R - fxParamA2 * localv1R;
		localv1R = localv0R;
		localv0R = temp;
		localV = (temp + (in - fxParamA1 * localv0R - fxParamA2 * localv1R)) * 0.5f;
		*sp++ = clamp((localV + fxParamB2 * localv1R) * mixerGain, -ratioTimbres, ratioTimbres);

		localv1R = localv0R;
		localv0R = localV;
	}

    v0L = localv0L;
    v0R = localv0R;
    v1L = localv1L;
    v1R = localv1R;

    break;
}
case FILTER_LP3:
{
	//https://www.musicdsp.org/en/latest/Filters/23-state-variable.html
	float fxParamTmp = SVFOFFSET + params.effect.param1 + matrixFilterFrequency;
	fxParamTmp *= fxParamTmp;
	// Low pass... on the Frequency
	fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

	const float f = fxParam1 * fxParam1 * SVFRANGE;
	const float fb = sqrt3(1 - fxParam2 * 0.999f);
	const float scale = sqrt3(fb);

	float *sp = this->sampleBlock;
	float lowL = v0L, highL = 0, bandL = v1L;
	float lowR = v0R, highR = 0, bandR = v1R;

	const float svfGain = (1 + SVFGAINOFFSET + fxParam2 * fxParam2 * 0.75f) * mixerGain;

	float _ly1L = v2L, _ly1R = v2R;
	float _lx1L = v3L, _lx1R = v3R;
	const float f1 = clamp(0.33f + f * 0.43f, filterWindowMin, filterWindowMax);
	float coef1 = (1.0f - f1) / (1.0f + f1);

	for (int k = BLOCK_SIZE; k--;)	{
		// Left voice

		_ly1L = coef1 * (_ly1L + *sp) - _lx1L; // allpass
		_lx1L = *sp;

		lowL += f * bandL;
		bandL += f * (scale * _ly1L - lowL - fb * sat25(bandL));

		lowL += f * bandL;
		bandL += f * (scale * _ly1L - lowL - fb * (bandL));

		*sp++ = clamp(lowL * svfGain, -ratioTimbres, ratioTimbres);

		// Right voice

		_ly1R = coef1 * (_ly1R + *sp) - _lx1R; // allpass
		_lx1R = *sp;

		lowR += f * bandR;
		bandR += f * (scale * _ly1R - lowR - fb * sat25(bandR));

		lowR += f * bandR;
		bandR += f * (scale * _ly1R - lowR - fb * (bandR));

		*sp++ = clamp(lowR * svfGain, -ratioTimbres, ratioTimbres);
	}

	v0L = lowL;
	v1L = bandL;
	v0R = lowR;
	v1R = bandR;

	v2L = _ly1L; v2R = _ly1R;
	v3L = _lx1L; v3R = _lx1R;
}
break;
case FILTER_HP3: 
{
	//https://www.musicdsp.org/en/latest/Filters/23-state-variable.html
	float fxParamTmp = SVFOFFSET + params.effect.param1 + matrixFilterFrequency;
	fxParamTmp *= fxParamTmp;
	// Low pass... on the Frequency
	fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

	const float f = fxParam1 * fxParam1 * 0.93f;
	const float fb = sqrt3(1 - fxParam2 * 0.999f);
	const float scale = sqrt3(fb);

	float *sp = this->sampleBlock;
	float lowL = v0L, highL = 0, bandL = v1L;
	float lowR = v0R, highR = 0, bandR = v1R;

	const float svfGain = (1 + SVFGAINOFFSET + fxParam2 * fxParam2 * 0.75f) * mixerGain;

	float _ly1L = v2L, _ly1R = v2R;
	float _lx1L = v3L, _lx1R = v3R;
	const float f1 = clamp(0.15f + f * 0.33f, filterWindowMin, filterWindowMax);
	float coef1 = (1.0f - f1) / (1.0f + f1);

	for (int k=BLOCK_SIZE ; k--; ) {
		// Left voice
		_ly1L = coef1 * (_ly1L + *sp) - _lx1L; // allpass
		_lx1L = *sp;

		lowL = lowL + f * bandL;
		highL = scale * (_ly1L) - lowL - fb * sat33(bandL);
		bandL = f * highL + bandL;

		lowL = lowL + f * bandL;
		highL = scale * (_ly1L) - lowL - fb * bandL;
		bandL = f * highL + bandL;

		*sp++ = clamp(highL * svfGain, -ratioTimbres, ratioTimbres);

		// Right voice
		_ly1R = coef1 * (_ly1R + *sp) - _lx1R; // allpass
		_lx1R = *sp;

		lowR = lowR + f * bandR;
		highR = scale * (_ly1R) - lowR - fb * sat33(bandR);
		bandR = f * highR + bandR;

		lowR = lowR + f * bandR;
		highR = scale * (_ly1R) - lowR - fb * bandR;
		bandR = f * highR + bandR;
		
		*sp++ = clamp(highR * svfGain, -ratioTimbres, ratioTimbres);
	}

	v0L = lowL;
	v1L = bandL;
	v0R = lowR;
	v1R = bandR;

	v2L = _ly1L; v2R = _ly1R;
	v3L = _lx1L; v3R = _lx1R;
}
break;
case FILTER_BP3: 
{
	//https://www.musicdsp.org/en/latest/Filters/23-state-variable.html
	float fxParamTmp = SVFOFFSET + params.effect.param1 + matrixFilterFrequency;
	fxParamTmp *= fxParamTmp;
	// Low pass... on the Frequency
	fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

	const float f = fxParam1 * fxParam1 * SVFRANGE;
	const float fb = sqrt3(0.5f - fxParam2 * 0.495f);
	const float scale = sqrt3(fb);

	float *sp = this->sampleBlock;
	float lowL = v0L, highL = 0, bandL = v1L;
	float lowR = v0R, highR = 0, bandR = v1R;

	const float svfGain = (1 + SVFGAINOFFSET + fxParam2 * fxParam2 * 0.75f) * mixerGain;

	float _ly1L = v2L, _ly1R = v2R;
	float _lx1L = v3L, _lx1R = v3R;
	float _ly2L = v4L, _ly2R = v4R;
	float _lx2L = v5L, _lx2R = v5R;
	const float f1 = clamp(f * 0.56f , filterWindowMin, filterWindowMax);
	float coef1 = (1.0f - f1) / (1.0f + f1);
	const float f2 = clamp(0.25f + f * 0.08f , filterWindowMin, filterWindowMax);
	float coef2 = (1.0f - f1) / (1.0f + f1);

	for (int k=BLOCK_SIZE ; k--; ) {

		// Left voice
		_ly1L = coef1 * (_ly1L + *sp) - _lx1L; // allpass
		_lx1L = *sp;

		lowL = lowL + f * bandL;
		highL = scale * _ly1L - lowL - fb * sat25(bandL);
		bandL = f * highL + bandL;
		
		_ly2L = coef2 * (_ly2L + bandL) - _lx2L; // allpass 2
		_lx2L = bandL;

		*sp++ = clamp(_ly2L * svfGain, -ratioTimbres, ratioTimbres);

		// Right voice
		_ly1R = coef1 * (_ly1R + *sp) - _lx1R; // allpass
		_lx1R = *sp;

		lowR = lowR + f * bandR;
		highR = scale * _ly1R - lowR - fb * sat25(bandR);
		bandR = f * highR + bandR;

		_ly2R = coef2 * (_ly2R + bandR) - _lx2R; // allpass 2
		_lx2R = bandR;

		*sp++ = clamp(_ly2R * svfGain, -ratioTimbres, ratioTimbres);
	}

	v0L = lowL;
	v1L = bandL;
	v0R = lowR;
	v1R = bandR;

	v2L = _ly1L; v2R = _ly1R;
	v3L = _lx1L; v3R = _lx1R;
	v4L = _ly2L; v4R = _ly2R;
	v5L = _lx2L; v5R = _lx2R;

}
break;
case FILTER_PEAK:
{
	//https://www.musicdsp.org/en/latest/Filters/23-state-variable.html
	float fxParamTmp = SVFOFFSET + params.effect.param1 + matrixFilterFrequency;
	fxParamTmp *= fxParamTmp;
	// Low pass... on the Frequency
	fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

	const float f = fxParam1 * fxParam1 * SVFRANGE;
	const float fb = sqrt3(1 - fxParam2 * 0.999f);
	const float scale = sqrt3(fb);

	float *sp = this->sampleBlock;
	float lowL = v0L, highL = 0, bandL = v1L;
	float lowR = v0R, highR = 0, bandR = v1R;

	const float svfGain = (1 + SVFGAINOFFSET + fxParam2 * fxParam2 * 0.75f) * mixerGain;

	float _ly1L = v2L, _ly1R = v2R;
	float _lx1L = v3L, _lx1R = v3R;
	const float f1 = clamp(0.27f + f * 0.33f, filterWindowMin, filterWindowMax);
	float coef1 = (1.0f - f1) / (1.0f + f1);

	float out;

	for (int k=BLOCK_SIZE ; k--; ) {

		// Left voice
		lowL = lowL + f * bandL;
		highL = scale * (*sp) - lowL - fb * bandL;
		bandL = f * highL + bandL;

		lowL = lowL + f * bandL;
		highL = scale * (*sp) - lowL - fb * sat33(bandL);
		bandL = f * highL + bandL;

		out = (bandL + highL + lowL);

		_ly1L = coef1 * (_ly1L + out) - _lx1L; // allpass
		_lx1L = out;

		*sp++ = clamp(_ly1L * svfGain, -ratioTimbres, ratioTimbres);

		// Right voice
		lowR = lowR + f * bandR;
		highR = scale * (*sp) - lowR - fb * bandR;
		bandR = f * highR + bandR;

		lowR = lowR + f * bandR;
		highR = scale * (*sp) - lowR - fb * sat33(bandR);
		bandR = f * highR + bandR;

		out = (bandR + highR + lowR);

		_ly1R = coef1 * (_ly1R + out) - _lx1R; // allpass
		_lx1R = out;

		*sp++ = clamp(_ly1R * svfGain, -ratioTimbres, ratioTimbres);
	}

	v0L = lowL;
	v1L = bandL;
	v0R = lowR;
	v1R = bandR;

	v2L = _ly1L; v2R = _ly1R;
	v3L = _lx1L; v3R = _lx1R;
}
break;
case FILTER_NOTCH:
{
	//https://www.musicdsp.org/en/latest/Filters/23-state-variable.html
	float fxParamTmp = SVFOFFSET + params.effect.param1 + matrixFilterFrequency;
	fxParamTmp *= fxParamTmp;
	// Low pass... on the Frequency
	fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

	const float f = fxParam1 * fxParam1 * SVFRANGE;
	const float fb = sqrt3(1 - fxParam2 * 0.6f);
	const float scale = sqrt3(fb);

	float *sp = this->sampleBlock;
	float lowL = v0L, highL = 0, bandL = v1L;
	float lowR = v0R, highR = 0, bandR = v1R;
	float notch;

	const float svfGain = (1 + SVFGAINOFFSET) * mixerGain;

	float _ly1L = v2L, _ly1R = v2R;
	float _lx1L = v3L, _lx1R = v3R;
	const float f1 = clamp(fxParam1 * 0.66f , filterWindowMin, filterWindowMax);
	float coef1 = (1.0f - f1) / (1.0f + f1);

	for (int k=0 ; k < BLOCK_SIZE; k++) {

		// Left voice
		lowL = lowL + f * bandL;
		highL = scale * (*sp) - lowL - fb * bandL;
		bandL = f * highL + bandL;
		notch = (highL + lowL);

		_ly1L = coef1 * (_ly1L + notch) - _lx1L; // allpass
		_lx1L = notch;

		*sp++ = clamp(_ly1L * svfGain, -ratioTimbres, ratioTimbres);

		// Right voice
		lowR = lowR + f * bandR;
		highR = scale * (*sp) - lowR - fb * bandR;
		bandR = f * highR + bandR;
		notch = (highR + lowR);

		_ly1R = coef1 * (_ly1R + notch) - _lx1R; // allpass
		_lx1R = notch;

		*sp++ = clamp(_ly1R * svfGain, -ratioTimbres, ratioTimbres);
	}

	v0L = lowL;
	v1L = bandL;
	v0R = lowR;
	v1R = bandR;

	v2L = _ly1L; v2R = _ly1R;
	v3L = _lx1L; v3R = _lx1R;
}
break;
case FILTER_BELL:
{
	//filter algo from Andrew Simper
	//https://cytomic.com/files/dsp/SvfLinearTrapOptimised2.pdf
	float fxParamTmp = SVFOFFSET + params.effect.param1 + matrixFilterFrequency;
	fxParamTmp *= fxParamTmp;
	// Low pass... on the Frequency
	fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

	//A = 10 ^ (db / 40)
	const float A = (tanh3(fxParam2 * 2) * 1.5f) + 0.5f;

	const float res = 0.6f;
	const float k = 1 / (0.0001f + res * A);
	const float g = 0.0001f + (fxParam1);
	const float a1 = 1 / (1 + g * (g + k));
	const float a2 = g * a1;
	const float a3 = g * a2;
	const float amp = k * (A * A - 1);

	float *sp = this->sampleBlock;

	float ic1eqL = v0L, ic2eqL = v1L;
	float ic1eqR = v0R, ic2eqR = v1R;
	float v1, v2, v3, out;

	float _ly1L = v2L, _ly1R = v2R;
	float _lx1L = v3L, _lx1R = v3R;
	const float f1 = clamp(0.25f + fxParam1 * 0.33f , filterWindowMin, filterWindowMax);
	float coef1 = (1.0f - f1) / (1.0f + f1);

	for (int k=BLOCK_SIZE ; k--; ) {

		// Left voice
		v3 = (*sp) - ic2eqL;
		v1 = a1 * ic1eqL + a2 * v3;
		v2 = ic2eqL + a2 * ic1eqL + a3 * v3;
		ic1eqL = 2 * v1 - ic1eqL;
		ic2eqL = 2 * v2 - ic2eqL;
		
		_ly1L = coef1 * (_ly1L + v1) - _lx1L; // allpass
		_lx1L = v1;

		out = (*sp + (amp * _ly1L));

		*sp++ = clamp(out * mixerGain, -ratioTimbres, ratioTimbres);

		// Right voice
		v3 = (*sp) - ic2eqR;
		v1 = a1 * ic1eqR + a2 * v3;
		v2 = ic2eqR + a2 * ic1eqR + a3 * v3;
		ic1eqR = 2 * v1 - ic1eqR;
		ic2eqR = 2 * v2 - ic2eqR;

		_ly1R = coef1 * (_ly1R + v1) - _lx1R; // allpass
		_lx1R = v1;

		out = (*sp + (amp * _ly1R));

		*sp++ = clamp(out * mixerGain, -ratioTimbres, ratioTimbres);
	}

	v0L = ic1eqL;
	v1L = ic2eqL;
	v0R = ic1eqR;
	v1R = ic2eqR;

	v2L = _ly1L; v2R = _ly1R;
	v3L = _lx1L; v3R = _lx1R;
}
break;
case FILTER_LOWSHELF:
{
	//filter algo from Andrew Simper
	//https://cytomic.com/files/dsp/SvfLinearTrapOptimised2.pdf
	float fxParamTmp = SVFOFFSET + params.effect.param1 + matrixFilterFrequency;
	fxParamTmp *= fxParamTmp;
	// Low pass... on the Frequency
	fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

	//A = 10 ^ (db / 40)
	const float A = (tanh3(fxParam2 * 2) * 1) + 0.5f;

	const float res = 0.5f;
	const float k = 1 / (0.0001f + res);
	const float g = 0.0001f + (fxParam1);
	const float a1 = 1 / (1 + g * (g + k));
	const float a2 = g * a1;
	const float a3 = g * a2;
	const float m1 = k * (A - 1);
	const float m2 = (A * A - 1);

	float *sp = this->sampleBlock;

	float ic1eqL = v0L, ic2eqL = v1L;
	float ic1eqR = v0R, ic2eqR = v1R;
	float v1, v2, v3, out;

	const float svfGain = mixerGain * 0.8f;

	float _ly1L = v2L, _ly1R = v2R;
	float _lx1L = v3L, _lx1R = v3R;
	const float f1 = clamp(0.33f + fxParam1 * 0.33f , filterWindowMin, filterWindowMax);
	float coef1 = (1.0f - f1) / (1.0f + f1);

	for (int k=BLOCK_SIZE ; k--; ) {

		// Left voice
		v3 = (*sp) - ic2eqL;
		v1 = a1 * ic1eqL + a2 * v3;
		v2 = ic2eqL + a2 * ic1eqL + a3 * v3;
		ic1eqL = 2 * v1 - ic1eqL;
		ic2eqL = 2 * v2 - ic2eqL;
		out = (*sp + (m1 * v1 + m2 * v2));
		_ly1L = coef1 * (_ly1L + out) - _lx1L; // allpass
		_lx1L = out;

		*sp++ = clamp(_ly1L * svfGain, -ratioTimbres, ratioTimbres);

		// Right voice
		v3 = (*sp) - ic2eqR;
		v1 = a1 * ic1eqR + a2 * v3;
		v2 = ic2eqR + a2 * ic1eqR + a3 * v3;
		ic1eqR = 2 * v1 - ic1eqR;
		ic2eqR = 2 * v2 - ic2eqR;
		out = (*sp + (m1 * v1 + m2 * v2));

		_ly1R = coef1 * (_ly1R + out) - _lx1R; // allpass
		_lx1R = out;

		*sp++ = clamp(_ly1R * svfGain, -ratioTimbres, ratioTimbres);
	}

	v0L = ic1eqL;
	v1L = ic2eqL;
	v0R = ic1eqR;
	v1R = ic2eqR;

	v2L = _ly1L; v2R = _ly1R;
	v3L = _lx1L; v3R = _lx1R;
}
break;
case FILTER_HIGHSHELF:
{
	//filter algo from Andrew Simper
	//https://cytomic.com/files/dsp/SvfLinearTrapOptimised2.pdf
	float fxParamTmp = SVFOFFSET + params.effect.param1 + matrixFilterFrequency;
	fxParamTmp *= fxParamTmp;
	// Low pass... on the Frequency
	fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

	//A = 10 ^ (db / 40)
	const float A = (tanh3(fxParam2 * 2) * 1) + 0.5f;

	const float res = 0.5f;
	const float k = 1 / (0.0001f + res);
	const float g = 0.0001f + (fxParam1);
	const float a1 = 1 / (1 + g * (g + k));
	const float a2 = g * a1;
	const float a3 = g * a2;
	const float m0 = A * A;
	const float m1 = k * (A - 1) * A;
	const float m2 = (1 - A * A);

	float *sp = this->sampleBlock;

	float ic1eqL = v0L, ic2eqL = v1L;
	float ic1eqR = v0R, ic2eqR = v1R;
	float v1, v2, v3, out;

	const float svfGain = mixerGain * 0.8f;

	float _ly1L = v2L, _ly1R = v2R;
	float _lx1L = v3L, _lx1R = v3R;
	const float f1 = clamp(0.33f + fxParam1 * 0.33f , filterWindowMin, filterWindowMax);
	float coef1 = (1.0f - f1) / (1.0f + f1);

	for (int k=BLOCK_SIZE ; k--; ) {

		// Left voice
		v3 = (*sp) - ic2eqL;
		v1 = a1 * ic1eqL + a2 * v3;
		v2 = ic2eqL + a2 * ic1eqL + a3 * v3;
		ic1eqL = 2 * v1 - ic1eqL;
		ic2eqL = 2 * v2 - ic2eqL;
		
		out = (m0 * *sp + (m1 * v1 + m2 * v2));
		_ly1L = coef1 * (_ly1L + out) - _lx1L; // allpass
		_lx1L = out;

		*sp++ = clamp(_ly1L * svfGain, -ratioTimbres, ratioTimbres);

		// Right voice
		v3 = (*sp) - ic2eqR;
		v1 = a1 * ic1eqR + a2 * v3;
		v2 = ic2eqR + a2 * ic1eqR + a3 * v3;
		ic1eqR = 2 * v1 - ic1eqR;
		ic2eqR = 2 * v2 - ic2eqR;

		out = (m0 * *sp + (m1 * v1 + m2 * v2));
		_ly1R = coef1 * (_ly1R + out) - _lx1R; // allpass
		_lx1R = out;

		*sp++ = clamp(_ly1R * svfGain, -ratioTimbres, ratioTimbres);
	}

	v0L = ic1eqL;
	v1L = ic2eqL;
	v0R = ic1eqR;
	v1R = ic2eqR;

	v2L = _ly1L; v2R = _ly1R;
	v3L = _lx1L; v3R = _lx1R;
}
break;
case FILTER_LPHP:
{
	float fxParamTmp = params.effect.param1 + matrixFilterFrequency;
	fxParamTmp *= fxParamTmp;

	// Low pass... on the Frequency
	fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

	const int mixWet = (fxParam1 * 122);
	const float mixA = (1 + fxParam2 * 2) * panTable[122 - mixWet];
	const float mixB = 2.5f * panTable[5 + mixWet];

	const float f = fxParam1 * fxParam1 * 1.5f;
	const float pattern = (1 - fxParam2 * f);

	float *sp = this->sampleBlock;
	float localv0L = v0L;
	float localv1L = v1L;
	float localv0R = v0R;
	float localv1R = v1R;
	float out;

	float _ly1L = v2L, _ly1R = v2R;
	float _lx1L = v3L, _lx1R = v3R;

	const float f1 = clamp(0.27f + fxParam1 * 0.44f , filterWindowMin, filterWindowMax);
	float coef1 = (1.0f - f1) / (1.0f + f1);

	const float gain = mixerGain * 1.3f;

	for (int k = BLOCK_SIZE ; k--; ) {

		// Left voice

		_ly1L = coef1 * (_ly1L + (*sp)) - _lx1L; // allpass
		_lx1L = (*sp);

		localv0L = pattern * localv0L + (f * (-localv1L + _ly1L));
		localv1L = pattern * localv1L + f * localv0L;

		localv0L = pattern * localv0L + (f * (-localv1L + _ly1L));
		localv1L = pattern * localv1L + f * localv0L;

		out = sat33((localv1L * mixA) + ((_ly1L - localv1L) * mixB));

		*sp++ = clamp(out * gain, -ratioTimbres, ratioTimbres);

		// Right voice

		_ly1R = coef1 * (_ly1R + (*sp)) - _lx1R; // allpass
		_lx1R = (*sp);

		localv0R = pattern * localv0R + (f * (-localv1R + _ly1R));
		localv1R = pattern * localv1R + f * localv0R;
		
		localv0R = pattern * localv0R + (f * (-localv1R + _ly1R));
		localv1R = pattern * localv1R + f * localv0R;

		out = sat33((localv1R * mixA) + ((_ly1R - localv1R) * mixB));

		*sp++ = clamp(out * gain, -ratioTimbres, ratioTimbres);
	}
	v0L = localv0L;
	v1L = localv1L;
	v0R = localv0R;
	v1R = localv1R;

	v2L = _ly1L; v2R = _ly1R;
	v3L = _lx1L; v3R = _lx1R;
}
break;
case FILTER_BPds:
{
	float fxParamTmp = SVFOFFSET + params.effect.param1 + matrixFilterFrequency;
	fxParamTmp *= fxParamTmp;
	// Low pass... on the Frequency
	fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

	const float f = fxParam1 * fxParam1 * SVFRANGE;
	const float fb = sqrt3(0.5f - fxParam2 * 0.5f);
	const float scale = sqrt3(fb);

	float *sp = this->sampleBlock;
	float lowL = v0L, highL = 0, bandL = v1L;
	float lowR = v0R, highR = 0, bandR = v1R;

	const float svfGain = (1 + SVFGAINOFFSET + fxParam2 * fxParam2 * 0.75f) * mixerGain;

	const float sat = 1 + fxParam2;

	float _ly1L = v2L, _ly1R = v2R;
	float _lx1L = v3L, _lx1R = v3R;
	const float f1 = clamp(fxParam1 * 0.35f , filterWindowMin, filterWindowMax);
	float coef1 = (1.0f - f1) / (1.0f + f1);
	float outL = 0, outR = 0;

	const float r = 0.9840f;

	for (int k=BLOCK_SIZE ; k--; ) {

		// Left voice
		*sp = outL + ((-outL + *sp) * r);

		lowL = lowL + f * bandL;
		highL = scale * (*sp) - lowL - fb * bandL;
		bandL = (f * highL) + bandL;
		_ly1L = coef1 * (_ly1L + bandL) - _lx1L; // allpass
		_lx1L = bandL;
		
		outL = sat25(_ly1L * sat);
		*sp++ = clamp( outL * svfGain, -ratioTimbres, ratioTimbres);

		// Right voice
		*sp = outR + ((-outR + *sp) * r);
		
		lowR = lowR + f * bandR;
		highR = scale * (*sp) - lowR - fb * bandR;
		bandR = (f * highR) + bandR;
		_ly1R = coef1 * (_ly1R + bandR) - _lx1R; // allpass
		_lx1R = bandR;

		outR = sat25(_ly1R * sat);
		*sp++ = clamp( outR * svfGain, -ratioTimbres, ratioTimbres);
	}

	v0L = lowL;
	v1L = bandL;
	v0R = lowR;
	v1R = bandR;

	v2L = _ly1L; v2R = _ly1R;
	v3L = _lx1L; v3R = _lx1R;
}
break;
case FILTER_LPWS:
	{
		float fxParamTmp = params.effect.param1 + matrixFilterFrequency;
		fxParamTmp *= fxParamTmp;

		// Low pass... on the Frequency
		fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

		const float a = 1.f - fxParam1;
		const float b = 1.f - a;

		float *sp = this->sampleBlock;
		float localv0L = v0L;
		float localv0R = v0R;

		const int mixWet = (params.effect.param2 * 127);
		const float mixA = panTable[mixWet] * mixerGain;
		const float mixB = panTable[127 - mixWet] * mixerGain;

		float _ly1L = v2L, _ly1R = v2R;
		float _lx1L = v3L, _lx1R = v3R;
		const float f1 = 0.27f + fxParam1 * 0.027;//clamp(fxParam1 - 0.26f , filterWindowMin, filterWindowMax);
		float coef1 = (1.0f - f1) / (1.0f + f1);

		for (int k=BLOCK_SIZE ; k--; ) {
			// Left voice
			localv0L = (*sp  * b) + (localv0L * a);

			_ly1L = coef1 * (_ly1L + localv0L) - _lx1L; // allpass
			_lx1L = localv0L;

			*sp++ = clamp((sigmoid(sat25(_ly1L * 2.0f)) * mixA + (mixB * (*sp))), -ratioTimbres, ratioTimbres);

			// Right voice
			localv0R = (*sp  * b) + (localv0R * a);

			_ly1R = coef1 * (_ly1R + localv0R) - _lx1R; // allpass
			_lx1R = localv0R;

			*sp++ = clamp((sigmoid(sat25(_ly1R * 2.0f)) * mixA + (mixB * (*sp))), -ratioTimbres, ratioTimbres);
		}
    	v0L = localv0L;
    	v0R = localv0R;

		v2L = _ly1L; v2R = _ly1R;
		v3L = _lx1L; v3R = _lx1R;
	}
	break;
case FILTER_TILT:
    {
		float fxParamTmp = params.effect.param1 + matrixFilterFrequency;
		fxParamTmp *= fxParamTmp;

		// Low pass... on the Frequency
		fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

		const float f1 = clamp(fxParam1 * 0.24f + 0.09f , filterWindowMin, filterWindowMax);
		float coef1 = (1.0f - f1) / (1.0f + f1);

		const float res = 0.85f;

		const float amp = 19.93f;
		const float gain = (params.effect.param2 - 0.5f);
		const float gfactor = 10;
		float g1, g2;
		if (gain > 0) {
			g1 = -gfactor * gain;
			g2 = gain;
		} else {
			g1 = -gain;
			g2 = gfactor * gain;
		};

		//two separate gains
		const float lgain = expf_fast(g1 / amp) - 1.f;
		const float hgain = expf_fast(g2 / amp) - 1.f;

		float *sp = this->sampleBlock;
		float localv0L = v0L;
		float localv0R = v0R;
		float localv1L = v1L;
		float localv1R = v1R;

		float _ly1L = v2L, _ly1R = v2R;
		float _ly2L = v3L, _ly2R = v3R;
		float _lx1L = v4L, _lx1R = v4R;

		float out;

		for (int k=BLOCK_SIZE ; k--; ) {
			// Left voice
			localv0L = res * localv0L - fxParam1 * localv1L + sigmoid(*sp);
			localv1L = res * localv1L + fxParam1 * localv0L;

			out = (*sp + lgain * localv1L + hgain * (*sp - localv1L));

			_ly1L = coef1 * (_ly1L + out) - _lx1L; // allpass
			_lx1L = out;

			*sp++ = clamp(_ly1L * mixerGain, -ratioTimbres, ratioTimbres);

			// Right voice
			localv0R = res * localv0R - fxParam1 * localv1R + sigmoid(*sp);
			localv1R = res * localv1R + fxParam1 * localv0R;

			out = (*sp + lgain * localv1R + hgain * (*sp - localv1R));

			_ly1R = coef1 * (_ly1R + out) - _lx1R; // allpass
			_lx1R = out;

			*sp++ = clamp(_ly1R * mixerGain, -ratioTimbres, ratioTimbres);
		}

        v0L = localv0L;
        v0R = localv0R;
        v1L = localv1L;
        v1R = localv1R;

		v2L = _ly1L; v2R = _ly1R;
		v3L = _ly2L; v3R = _ly2R;
		v4L = _lx1L; v4R = _lx1R;

    }
    break;
case FILTER_STEREO:
{
	float fxParamTmp = fabsf(params.effect.param1 + matrixFilterFrequency);
	fxParam1 = ((fxParamTmp + 19.0f * fxParam1) * .05f);

	float OffsetTmp = fabsf(params.effect.param2);
	fxParam2 = ((OffsetTmp + 19.0f * fxParam2) * .05f);

	const float offset = fxParam2 * 0.66f - 0.33f;

	const float f1 = clamp((fxParam1), 0.001f, 0.99f) * 1.8f - 0.9f;
	const float f2 = clamp((fxParam1 + offset), 0.001f, 0.99f) * 1.8f - 0.9f;
	const float f3 = clamp((fxParam1 + offset * 2), 0.001f, 0.99f) * 1.8f - 0.9f;;

	float *sp = this->sampleBlock;
	float lowL = v0L, highL = 0, bandL = v1L;
	float lowR = v0R, highR = 0, bandR = v1R;
	float lowL2 = v2L, highL2 = 0, bandL2 = v3L;
	float lowR2 = v2R, highR2 = 0, bandR2 = v3R;
	float lowL3 = v4L, highL3 = 0, bandL3 = v5L;
	float lowR3 = v4R, highR3 = 0, bandR3 = v5R;
	float out;

	for (int k = BLOCK_SIZE ; k--; ) {
		// Left voice
		
		lowL = (*sp) + f1 * (bandL = lowL - f1 * (*sp));
		highL = (bandL + (*sp)) * 0.5f;

		lowL2 = highL + f2 * (bandL2 = (lowL2) - f2 * lowL);
		highL2 = ((bandL2) + highL) * 0.5f;

		lowL3 = lowL2 + f3 * (bandL3 = lowL3 - f3 * lowL2);
		highL3 = (bandL3 + highL2) * 0.5f;

		*sp++ = clamp(highL3 * mixerGain, -ratioTimbres, ratioTimbres);

		// Right voice
		lowR = (*sp) + f1 * (bandR = lowR - f1 * (*sp));
		highR = (bandR + (*sp)) * 0.5f;

		lowR2 = lowR + f2 * (bandR2 = (lowR2) - f2 * lowR);
		highR2 = ((bandR2) + highR) * 0.5f;

		lowR3 = lowR2 + f3 * (bandR3 = lowR3 - f3 * lowR2);
		highR3 = (bandR3 + highR2) * 0.5f;

		*sp++ = clamp(highR3 * mixerGain, -ratioTimbres, ratioTimbres);
	}

	v0L = lowL;
	v1L = bandL;
	v0R = lowR;
	v1R = bandR;

	v2L = lowL2;
	v3L = bandL2;
	v2R = lowR2;
	v3R = bandR2;

	v4L = lowL3;
	v5L = bandL3;
	v4R = lowR3;
	v5R = bandR3;
}
break;
case FILTER_SAT:
	{
		float fxParamTmp = params.effect.param1 + matrixFilterFrequency;
		fxParamTmp *= fxParamTmp;

		// Low pass... on the Frequency
		fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

		const float f1 = clamp((fxParam1), 0.001f, 1); // allpass F
		float coef1 = (1.0f - f1) / (1.0f + f1);

		float *sp = this->sampleBlock;

		float localv0L = v0L;
		float localv0R = v0R;

		const float a = 0.95f - fxParam2 * 0.599999f;
		const float b = 1.f - a;

		const float threshold = (sqrt3(fxParam1) * 0.4f) * numberVoicesAttn;
		const float thresTop = (threshold + 1) * 0.5f;
		const float invT = 1.f / thresTop;

		for (int k=BLOCK_SIZE ; k--; ) {
			//LEFT
			localv0L = sigmoid(*sp);
			if(fabsf(localv0L) > threshold) {
				localv0L = localv0L > 1 ? thresTop : localv0L * invT;
			}
			localv0L = (*sp * b) + (localv0L * a);
			*sp++ = clamp((*sp - localv0L) * mixerGain, -ratioTimbres, ratioTimbres);

			//RIGHT
			localv0R = sigmoid(*sp);
			if(fabsf(localv0R) > threshold) {
				localv0R = localv0R > 1 ? thresTop : localv0R * invT;
			}
			localv0R = (*sp * b) + (localv0R * a);
			*sp++ = clamp((*sp - localv0R) * mixerGain, -ratioTimbres, ratioTimbres);
		}
    	v0L = localv0L;
    	v0R = localv0R;
	}
	break;
case FILTER_SIGMOID:
	{
		float fxParamTmp = params.effect.param1 + matrixFilterFrequency;
		fxParamTmp *= fxParamTmp;

		// Low pass... on the Frequency
		fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

		float *sp = this->sampleBlock;
		float localv0L = v0L;
		float localv1L = v1L;
		float localv0R = v0R;
		float localv1R = v1R;

		float f = fxParam2 * 0.5f + 0.12f;
		float pattern = (1 - 0.7f * f);

		int drive = (27 + sqrt3(fxParam1) * 100);
		float gain = 1.1f + 44 * panTable[drive];
		float gainCorrection = (1.2f - sqrt3(panTable[64 + (drive >> 1)] * 0.6f));
		float bias = -0.1f + (fxParam1 * 0.2f);

		for (int k=BLOCK_SIZE ; k--; ) {
			// Left voice
			localv0L = tanh3(bias + sat33(*sp) * gain) * gainCorrection;
			localv0L = pattern * localv0L + f * (*sp - localv1L);
			localv1L = pattern * localv1L + f * localv0L;

			*sp++ = clamp((*sp - localv1L) * mixerGain, -ratioTimbres, ratioTimbres);

			// Right voice
			localv0R = tanh3(bias + sat33(*sp) * gain) * gainCorrection;
			localv0R = pattern * localv0R + f * (*sp - localv1R);
			localv1R = pattern * localv1R + f * localv0R;

			*sp++ = clamp((*sp - localv1R) * mixerGain, -ratioTimbres, ratioTimbres);
		}

    	v0L = localv0L;
    	v0R = localv0R;
        v1L = localv1L;
        v1R = localv1R;
	}
	break;
case FILTER_FOLD:
	{
		//https://www.desmos.com/calculator/ge2wvg2wgj

		float fxParamTmp = params.effect.param1 + matrixFilterFrequency;
		fxParamTmp *= fxParamTmp;

		// Low pass... on the Frequency
		fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

		float *sp = this->sampleBlock;
		float localv0L = v0L;
		float localv1L = v1L;
		float localv0R = v0R;
		float localv1R = v1R;

		float f = fxParam2;
		float f4 = f * 4;//optimisation
		float pattern = (1 - 0.6f * f);

		float drive = sqrt3(fxParam1);
		float gain = (1 + 52 * (drive)) * 0.25f;
		float finalGain = (1 - (drive / (drive + 0.05f)) * 0.7f) * mixerGain  * 0.95f;
	
		float _ly1L = v2L, _ly1R = v2R;
		float _lx1L = v3L, _lx1R = v3R;
		const float f1 = clamp(0.57f - fxParam1 * 0.43f, filterWindowMin, filterWindowMax);
		float coef1 = (1.0f - f1) / (1.0f + f1);

		for (int k=BLOCK_SIZE ; k--; ) {
			//LEFT
			_ly1L = coef1 * (_ly1L + *sp) - _lx1L; // allpass
			_lx1L = *sp;

			localv0L = pattern * localv0L - f * localv1L + f4 * fold(_ly1L * gain);
			localv1L = pattern * localv1L + f * localv0L;

			*sp++ = clamp(localv1L * finalGain, -ratioTimbres, ratioTimbres);

			//RIGHT
			_ly1R = coef1 * (_ly1R + *sp) - _lx1R; // allpass
			_lx1R = *sp;

			localv0R = pattern * localv0R - f * localv1R + f4 * fold(_ly1R * gain);
			localv1R = pattern * localv1R + f * localv0R;

			*sp++ = clamp(localv1R * finalGain, -ratioTimbres, ratioTimbres);
		}

        v0L = localv0L;
        v0R = localv0R;
        v1L = localv1L;
        v1R = localv1R;

		v2L = _ly1L; v2R = _ly1R;
		v3L = _lx1L; v3R = _lx1R;
	}
	break;
case FILTER_WRAP:
	{
		float fxParamTmp = params.effect.param1 + matrixFilterFrequency;
		fxParamTmp *= fxParamTmp;

		// Low pass... on the Frequency
		fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

		float *sp = this->sampleBlock;
		float localv0L = v0L;
		float localv1L = v1L;
		float localv0R = v0R;
		float localv1R = v1R;

		float f = fxParam2;
		float pattern = (1 - 0.6f * f);

		float drive = sqrt3(fxParam1);
		float gain = (1 + 4 * (drive));
		float finalGain = (1 - sqrt3(f * 0.5f) * 0.75f) * mixerGain * 0.85f;

		float _ly1L = v2L, _ly1R = v2R;
		float _lx1L = v3L, _lx1R = v3R;
		const float f1 = clamp(0.58f - fxParam1 * 0.43f, filterWindowMin, filterWindowMax);
		float coef1 = (1.0f - f1) / (1.0f + f1);

		for (int k=BLOCK_SIZE ; k--; ) {
			//LEFT
			localv0L = pattern * localv0L + f * (wrap(*sp * gain) - localv1L);
			localv1L = pattern * localv1L + f * localv0L;

			_ly1L = coef1 * (_ly1L + localv1L) - _lx1L; // allpass
			_lx1L = localv1L;
			*sp++ = clamp(_ly1L * finalGain, -ratioTimbres, ratioTimbres);

			//RIGHT
			localv0R = pattern * localv0R + f * (wrap(*sp * gain) - localv1R);
			localv1R = pattern * localv1R + f * localv0R;

			_ly1R = coef1 * (_ly1R + localv1R) - _lx1R; // allpass
			_lx1R = localv1R;
			*sp++ = clamp(_ly1R * finalGain, -ratioTimbres, ratioTimbres);
		}

        v0L = localv0L;
        v0R = localv0R;
        v1L = localv1L;
        v1R = localv1R;

		v2L = _ly1L; v2R = _ly1R;
		v3L = _lx1L; v3R = _lx1R;
	}
	break;
case FILTER_XOR:
		{
		float fxParamTmp = params.effect.param1 + matrixFilterFrequency;
		fxParamTmp *= fxParamTmp;

		// Low pass... on the Frequency
		fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

		float pos;
		float p1sq = sqrt3(fxParam1);
		if(p1sq > 0.5f) {
			pos = ((1 - panTable[127 - (int)(p1sq * 64)]) * 2) - 1;
		} else {
			pos = (panTable[(int)(p1sq * 64)] * 2) - 1;
		}

		float *sp = this->sampleBlock;
		float localv0L = v0L;
		float localv0R = v0R;

		const float a = 0.95f - fxParam2 * 0.5f;
		const float b = 1 - a;

		int digitsA, digitsB;
		const float threshold = (0.66f - p1sq * 0.66f) * numberVoicesAttn;

		float _ly2L = v1L, _ly2R = v1R;
		float _lx2L = v5L, _lx2R = v5R;

		const float f2 = clamp(0.33f + fxParam1 * 0.5f, filterWindowMin, filterWindowMax);
		float coef2 = (1.0f - f2) / (1.0f + f2);

		for (int k=BLOCK_SIZE ; k--; ) {
			// Left
			if(fabsf(*sp) > threshold) {
				localv0L = (*sp) - pos * (localv0L + pos * (*sp));
				digitsA = FLOAT2SHORT * (*sp);
				digitsB = FLOAT2SHORT * localv0L;
				localv0L = SHORT2FLOAT * roundf(digitsA ^ digitsB & 0xfff);
			} else {
				localv0L = *sp;
			}

			localv0L = (*sp * b) + (localv0L * a);

			_ly2L = coef2 * (_ly2L + localv0L) - _lx2L; // allpass
			_lx2L = localv0L;

			*sp++ = clamp(_ly2L * mixerGain, -ratioTimbres, ratioTimbres);

			// Right
			if(fabsf(*sp) > threshold) {
				localv0R = (*sp) - pos * (localv0R + pos * (*sp));
				digitsA = FLOAT2SHORT * (*sp);
				digitsB = FLOAT2SHORT * localv0R;
				localv0R = SHORT2FLOAT * roundf(digitsA ^ digitsB & 0xfff);
			} else {
				localv0R = *sp;
			}

			localv0R = (*sp  * b) + (localv0R * a);

			_ly2R = coef2 * (_ly2R + localv0R) - _lx2R; // allpass
			_lx2R = localv0R;

			*sp++ = clamp(_ly2R * mixerGain, -ratioTimbres, ratioTimbres);
		}

		v0L = localv0L;
		v0R = localv0R;

		v1L = _ly2L; v1R = _ly2R;
		v5L = _lx2L; v5R = _lx2R;
	}
	break;
case FILTER_TEXTURE1:
	{
		float fxParamTmp = (params.effect.param1 + matrixFilterFrequency);
		fxParamTmp *= fxParamTmp;

		const uint_fast8_t random = (*(uint_fast8_t *)noise) & 0xff;
		if (random > 252) {
			fxParam1 += ((random & 1) * 0.007874015748031f);
		}
		
		// Low pass... on the Frequency
		fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

		float *sp = this->sampleBlock;
		float lowL = v0L, highL = 0, bandL = v1L;
		float lowR = v0R, highR = 0, bandR = v1R;
		float notch;

		const float f = (fxParam1 * fxParam1 * fxParam1) * 0.5f;
		const float fb = fxParam2;
		const float scale = sqrt3(fb);

		const int highBits = 0xFFFFFD4F;
		const int lowBits = ~(highBits);

		const int ll = (int)(fxParam1 * lowBits);
		int digitsL, digitsR;
		int lowDigitsL, lowDigitsR;

		float _ly1L = v2L, _ly1R = v2R;
		float _lx1L = v3L, _lx1R = v3R;
		const float f1 = clamp(0.33f + f * 0.47f, filterWindowMin, filterWindowMax);
		float coef1 = (1.0f - f1) / (1.0f + f1);

		//const float r = 0.9940f;

		for (int k=BLOCK_SIZE ; k--; ) {
			//LEFT
			//*sp = _ly1L + ((-_ly1L + *sp) * r);

			lowL = lowL + f * bandL;
			highL = scale * (*sp) - lowL - fb * bandL;
			bandL = f * highL + bandL;
			notch = lowL + highL;

			digitsL = FLOAT2SHORT * bandL;
			lowDigitsL = (digitsL & lowBits);
			bandL = SHORT2FLOAT * (float)((digitsL & highBits) ^ ((lowDigitsL ^ ll) & 0x1FFF));

			lowL = lowL + f * bandL;
			highL = scale * (notch) - lowL - fb * bandL;
			bandL = f * highL + bandL;

			_ly1L = coef1 * (_ly1L + notch) - _lx1L; // allpass
			_lx1L = notch;

			*sp++ = clamp(_ly1L * mixerGain, -ratioTimbres, ratioTimbres);

			//RIGHT
			//*sp = _ly1R + ((-_ly1R + *sp) * r);

			lowR = lowR + f * bandR;
			highR = scale * (*sp) - lowR - fb * bandR;
			bandR = f * highR + bandR;
			notch = lowR + highR;

			digitsR = FLOAT2SHORT * bandR;
			lowDigitsR = (digitsR & lowBits);
			bandR = SHORT2FLOAT * (float)((digitsR & highBits) ^ ((lowDigitsR ^ ll) & 0x1FFF));

			lowR = lowR + f * bandR;
			highR = scale * (notch) - lowR - fb * bandR;
			bandR = f * highR + bandR;

			_ly1R = coef1 * (_ly1R + notch) - _lx1R; // allpass
			_lx1R = notch;

			*sp++ = clamp(_ly1R * mixerGain, -ratioTimbres, ratioTimbres);
		}

		v0L = lowL;
		v1L = bandL;
		v0R = lowR;
		v1R = bandR;

		v2L = _ly1L; v2R = _ly1R;
		v3L = _lx1L; v3R = _lx1R;
	}
    break;
case FILTER_TEXTURE2:
    {
		float fxParamTmp = (params.effect.param1 + matrixFilterFrequency);
		fxParamTmp *= fxParamTmp;

		const uint_fast8_t random = (*(uint_fast8_t *)noise) & 0xff;
		if (random > 252) {
			fxParam1 += ((random & 1) * 0.007874015748031f);
		}

		// Low pass... on the Frequency
		fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

		float *sp = this->sampleBlock;
		float lowL = v0L, highL = 0, bandL = v1L;
		float lowR = v0R, highR = 0, bandR = v1R;
		float notch;

		const float f = (fxParam1 * fxParam1 * fxParam1) * 0.5f;
		const float fb = fxParam2;
		const float scale = sqrt3(fb);

		const int highBits = 0xFFFFFFAF;
		const int lowBits = ~(highBits);

		const int ll = fxParam1 * lowBits;
		int digitsL, digitsR;
		int lowDigitsL, lowDigitsR;

		float _ly1L = v2L, _ly1R = v2R;
		float _lx1L = v3L, _lx1R = v3R;
		float _ly2L = v4L, _ly2R = v4R;
		float _lx2L = v5L, _lx2R = v5R;
		const float f1 = clamp(0.57f - fxParam1 * 0.33f, filterWindowMin, filterWindowMax);
		float coef1 = (1.0f - f1) / (1.0f + f1);
		const float f2 = clamp(0.50f - fxParam1 * 0.39f , filterWindowMin, filterWindowMax);
		float coef2 = (1.0f - f1) / (1.0f + f1);

		const float r = 0.9940f;

		for (int k=BLOCK_SIZE ; k--; ) {
			//LEFT
			*sp = _ly2L + ((-_ly2L + *sp) * r);

			lowL = lowL + f * bandL;
			highL = scale * (*sp) - lowL - fb * bandL;
			bandL = f * highL + bandL;
			notch = lowL + highL;

			digitsL = FLOAT2SHORT * (bandL);
			lowDigitsL = (digitsL & lowBits);
			bandL = SHORT2FLOAT * (float)((digitsL & highBits) ^ ((lowDigitsL * ll) & 0x1FFF));

			_ly1L = coef1 * (_ly1L + notch) - _lx1L; // allpass
			_lx1L = notch;
			
			/*lowL = lowL + f * bandL;
			highL = scale * (_ly1L) - lowL - fb * bandL;
			bandL = f * highL + bandL;*/

			_ly2L = coef2 * (_ly2L + _ly1L) - _lx2L; // allpass 2
			_lx2L = _ly1L;

			*sp++ = clamp(_ly2L * mixerGain, -ratioTimbres, ratioTimbres);

			//RIGHT
			*sp = _ly2R + ((-_ly2R + *sp) * r);

			lowR = lowR + f * bandR;
			highR = scale * (*sp) - lowR - fb * bandR;
			bandR = f * highR + bandR;
			notch = lowR + highR;

			digitsR = FLOAT2SHORT * (bandR);
			lowDigitsR = (digitsR & lowBits);
			bandR = SHORT2FLOAT * (float)((digitsR & highBits) ^ ((lowDigitsR * ll) & 0x1FFF));

			_ly1R = coef1 * (_ly1R + notch) - _lx1R; // allpass
			_lx1R = notch;

			/*lowR = lowR + f * bandR;
			highR = scale * (notch) - lowR - fb * bandR;
			bandR = f * highR + bandR;*/

			_ly2R = coef2 * (_ly2R + _ly1R) - _lx2R; // allpass 2
			_lx2R = _ly1R;

			*sp++ = clamp(_ly2R * mixerGain, -ratioTimbres, ratioTimbres);
		}

		v0L = lowL;
		v1L = bandL;
		v0R = lowR;
		v1R = bandR;

		v2L = _ly1L; v2R = _ly1R;
		v3L = _lx1L; v3R = _lx1R;
		v4L = _ly2L; v4R = _ly2R;
		v5L = _lx2L; v5R = _lx2R;
    }
    break;
case FILTER_LPXOR:
	{
		float fxParamTmp = params.effect.param1 + matrixFilterFrequency;
		fxParamTmp *= fxParamTmp;

		const uint_fast8_t random = (*(uint_fast8_t *)noise) & 0xff;
		if (random > 252) {
			fxParam1 += ((random & 1) * 0.007874015748031f);
		}

		// Low pass... on the Frequency
		fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

		const float a = 1.f - fxParam1;
		const float a2f = a * SHORT2FLOAT;
		const float b = 1.f - a;

		float *sp = this->sampleBlock;
		float localv0L = v0L;
		float localv0R = v0R;
		float digitized;

		float drive = (fxParam2 * fxParam2);
		float gain = (1 + 2 * drive);

		int digitsAL, digitsBL, digitsAR, digitsBR;
		digitsAL = digitsBL = digitsAR = digitsBR = 0;

		const short bitmask = 0xfac;

		float _ly1L = v2L, _ly1R = v2R;
		float _lx1L = v3L, _lx1R = v3R;
		const float f1 = clamp(0.27f + fxParam1 * 0.5f, filterWindowMin, filterWindowMax);
		float coef1 = (1.0f - f1) / (1.0f + f1);

		const float r = 0.9940f;

		for (int k = BLOCK_SIZE; k--;) {
			// Left voice
			*sp = _ly1L + ((-_ly1L + *sp) * r);

			digitsAL = FLOAT2SHORT * fold(localv0L * gain);
			digitsBL = FLOAT2SHORT * (*sp);
			digitized = (float)(digitsAL ^ (digitsBL & bitmask));
			localv0L = (*sp * b) + (digitized * a2f);

			_ly1L = coef1 * (_ly1L + localv0L) - _lx1L; // allpass
			_lx1L = localv0L;

			*sp++ = clamp(_ly1L * mixerGain, -ratioTimbres, ratioTimbres);

			// Right voice
			*sp = _ly1R + ((-_ly1R + *sp) * r);

			digitsAR = FLOAT2SHORT * fold(localv0R * gain);
			digitsBR = FLOAT2SHORT * (*sp);
			digitized = (float)(digitsAR ^ (digitsBR & bitmask));
			localv0R = (*sp * b) + (digitized * a2f);

			_ly1R = coef1 * (_ly1R + localv0R) - _lx1R; // allpass
			_lx1R = localv0R;

			*sp++ = clamp(_ly1R * mixerGain, -ratioTimbres, ratioTimbres);
		}
    	v0L = localv0L;
    	v0R = localv0R;

		v2L = _ly1L; v2R = _ly1R;
		v3L = _lx1L; v3R = _lx1R;
	}
	break;
case FILTER_LPXOR2:
	{
		float fxParamTmp = params.effect.param1 + matrixFilterFrequency;
		fxParamTmp *= fxParamTmp;

		const uint8_t random = (*(uint8_t *)noise) & 0xff;
		if (random > 252) {
			fxParam1 += ((random & 1) * 0.007874015748031f);
		}

		// Low pass... on the Frequency
		fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

		const float a = 1.f - fxParam1;
		const float a2f = a * SHORT2FLOAT;
		const float b = 1.f - a;

		float *sp = this->sampleBlock;
		float localv0L = v0L;
		float localv0R = v0R;
		float digitized;

		float drive = (fxParam2 * fxParam2);
		float gain = (1 + 8 * (drive)) * 0.25f;

		int digitsAL, digitsBL, digitsAR, digitsBR;
		digitsAL = digitsBL = digitsAR = digitsBR = 0;

		const short bitmask = 0xfba - (int)(fxParam1 * 7);

		float _ly1L = v2L, _ly1R = v2R;
		float _lx1L = v3L, _lx1R = v3R;
		const float f1 = clamp(0.27f + fxParam1 * 0.4f, filterWindowMin, filterWindowMax);
		float coef1 = (1.0f - f1) / (1.0f + f1);

		const float r = 0.9940f;

		for (int k=BLOCK_SIZE ; k--; ) {
			// Left voice
			*sp = _ly1L + ((-_ly1L + *sp) * r);

			digitsAL = FLOAT2SHORT * localv0L;
			digitsBL = FLOAT2SHORT * fold(*sp * gain);
			digitized = ((digitsAL | (digitsBL & bitmask)));
			localv0L = (*sp * b) + (digitized * a2f);

			_ly1L = coef1 * (_ly1L + localv0L) - _lx1L; // allpass
			_lx1L = localv0L;

			*sp++ = clamp(_ly1L * mixerGain, -ratioTimbres, ratioTimbres);

			// Right voice
			*sp = _ly1R + ((-_ly1R + *sp) * r);

			digitsAR = FLOAT2SHORT * localv0R;
			digitsBR = FLOAT2SHORT * fold(*sp * gain);
			digitized = ((digitsAR | (digitsBR & bitmask)));
			localv0R = (*sp * b) + (digitized * a2f);

			_ly1R = coef1 * (_ly1R + localv0R) - _lx1R; // allpass
			_lx1R = localv0R;

			*sp++ = clamp(_ly1R * mixerGain, -ratioTimbres, ratioTimbres);
		}
    	v0L = localv0L;
    	v0R = localv0R;

		v2L = _ly1L; v2R = _ly1R;
		v3L = _lx1L; v3R = _lx1R;
	}
	break;
case FILTER_LPSIN:
    {
	float fxParamTmp = SVFOFFSET + params.effect.param1 + matrixFilterFrequency;
	fxParamTmp *= fxParamTmp;
	// Low pass... on the Frequency
	fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

	const float f = fxParam2 * fxParam2 * fxParam2;
	const float fb = 0.45f;
	const float scale = sqrt3(fb);
	const int pos = (int)(fxParam1 * 2060);

	float *sp = this->sampleBlock;
	float lowL = v0L, highL = 0, bandL = v1L;
	float lowR = v0R, highR = 0, bandR = v1R;

	const float svfGain = (1 + SVFGAINOFFSET + fxParam2 * fxParam2 * 0.75f) * mixerGain;
	float drive = fxParam1 * fxParam1 * 0.25f;
	
	for (int k = BLOCK_SIZE ; k--; ) {
		// Left voice
		*sp = lowL + ((-lowL + *sp) * 0.9840f);

		lowL = lowL + f * bandL;
		highL = scale * satSin(*sp, drive, pos) - lowL - fb * bandL;
		bandL = f * highL + bandL;

		*sp++ = clamp( lowL * svfGain, -ratioTimbres, ratioTimbres);

		// Right voice
		*sp = lowR + ((-lowR + *sp) * 0.9840f);

		lowR = lowR + f * bandR;
		highR = scale * satSin(*sp, drive, pos) - lowR - fb * bandR;
		bandR = f * highR + bandR;

		*sp++ = clamp( lowR * svfGain, -ratioTimbres, ratioTimbres);
	}

	v0L = lowL;
	v1L = bandL;
	v0R = lowR;
	v1R = bandR;
    }
    break;
case FILTER_HPSIN:
    {
	float fxParamTmp = SVFOFFSET + params.effect.param1 + matrixFilterFrequency;
	fxParamTmp *= fxParamTmp;
	// Low pass... on the Frequency
	fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

	const float f = fxParam2 * fxParam2 * fxParam2;
	const float fb = 0.94f;
	const float scale = sqrt3(fb);
	const int pos = (int)(fxParam1 * 2060);

	float *sp = this->sampleBlock;
	float lowL = v0L, highL = 0, bandL = v1L;
	float lowR = v0R, highR = 0, bandR = v1R;

	const float svfGain = (1 + SVFGAINOFFSET + fxParam2 * fxParam2 * 0.75f) * mixerGain;
	float drive = fxParam1 * fxParam1 * 0.25f;
	
	for (int k = BLOCK_SIZE ; k--; ) {
		// Left voice
		*sp = highL + ((-highL + *sp) * 0.9840f);

		lowL = lowL + f * bandL;
		highL = scale * satSin(*sp, drive, pos) - lowL - fb * bandL;
		bandL = f * highL + bandL;

		*sp++ = clamp( highL * svfGain, -ratioTimbres, ratioTimbres);

		// Right voice
		*sp = highR + ((-highR + *sp) * 0.9840f);

		lowR = lowR + f * bandR;
		highR = scale * satSin(*sp, drive, pos) - lowR - fb * bandR;
		bandR = f * highR + bandR;

		*sp++ = clamp( highR * svfGain, -ratioTimbres, ratioTimbres);
	}

	v0L = lowL;
	v1L = bandL;
	v0R = lowR;
	v1R = bandR;
    }
    break;
case FILTER_QUADNOTCH:
{
	//https://www.musicdsp.org/en/latest/Filters/23-state-variable.html
	float fxParamTmp = (params.effect.param1 + matrixFilterFrequency);
	fxParamTmp *= fxParamTmp;

	const float bipolarf = (fxParam1 - 0.5f);
	const float folded = fold(sigmoid(bipolarf * 13 * fxParam2)) * 4; // - < folded < 1

	fxParam1 = ((fxParamTmp + 9.0f * fxParam1) * .1f);
	
	float OffsetTmp = fabsf(params.effect.param2);
	fxParam2 = ((OffsetTmp + 9.0f * fxParam2) * .1f);

	const float offset = fxParam2 * fxParam2 * 0.17f;
	const float spread = 0.8f + fxParam2 * 0.8f;
	const float lrDelta = 0.005f * folded;
	const float range = 0.47f;

	const float windowMin = 0.005f, windowMax = 0.99f;

	const float f1L = clamp(fold((fxParam1 - offset - lrDelta) * range) * 2, windowMin, windowMax);
	const float f2L = clamp(fold((fxParam1 + offset + lrDelta) * range) * 2, windowMin, windowMax);
	const float f3L = clamp(fold((fxParam1 - (offset * 2) - lrDelta) * range) * 2, windowMin, 1);
	const float f4L = clamp(fold((fxParam1 + (offset * 2) + lrDelta) * range) * 2, windowMin, 1);	
	const float f1R = clamp(fold((f1L - lrDelta) * range) * 2, windowMin, windowMax);
	const float f2R = clamp(fold((f2L + lrDelta) * range) * 2, windowMin, windowMax);
	const float f3R = clamp(fold((f3L - lrDelta) * range) * 2, windowMin, windowMax);
	const float f4R = clamp(fold((f3L + lrDelta) * range) * 2, windowMin, windowMax);

	float *sp = this->sampleBlock;
	float lowL = v0L, highL = 0, bandL = v1L;
	float lowR = v0R, highR = 0, bandR = v1R;
	float lowL2 = v2L, highL2 = 0, bandL2 = v3L;
	float lowR2 = v2R, highR2 = 0, bandR2 = v3R;
	float lowL3 = v4L, highL3 = 0, bandL3 = v5L;
	float lowR3 = v4R, highR3 = 0, bandR3 = v5R;
	float lowL4 = v6L, highL4 = 0, bandL4 = v7L;
	float lowR4 = v6R, highR4 = 0, bandR4 = v7R;

	for (int k = BLOCK_SIZE ; k--; ) {
		// Left voice
		*sp = (highL4 + lowL4) + ((-(highL4 + lowL4) + *sp) * 0.9840f);

		lowL = lowL + f1L * bandL;
		highL = (*sp) - lowL - bandL;
		bandL = f1L * highL + bandL;

		lowL2 = lowL2 + f2L * bandL2;
		highL2 = (highL + lowL) - lowL2 - bandL2;
		bandL2 = f2L * (highL2) + bandL2;

		lowL3 = lowL3 + f3L * bandL3;
		highL3 = (highL2 + lowL2) - lowL3 - bandL3;
		bandL3 = f3L * highL3 + bandL3;

		lowL4 = lowL4 + f4L * bandL4;
		highL4 = (highL3 + lowL3) - lowL4 - bandL4;
		bandL4 = f4L * highL4 + bandL4;

		*sp++ = clamp((highL4 + lowL4) * mixerGain, -ratioTimbres, ratioTimbres);

		// Right voice
		*sp = (highR4 + lowR4) + ((-(highR4 + lowR4) + *sp) * 0.9840f);

		lowR = lowR + f1R * bandR;
		highR = (*sp) - lowR - bandR;
		bandR = f1R * highR + bandR;

		lowR2 = lowR2 + f2R * bandR2;
		highR2 = (highR + lowR) - lowR2 - bandR2;
		bandR2 = f2R * (highR2) + bandR2;

		lowR3 = lowR3 + f3R * bandR3;
		highR3 = (highR2 + lowR2) - lowR3 - bandR3;
		bandR3 = f3R * highR3 + bandR3;

		lowR4 = lowR4 + f4R * bandR4;
		highR4 = (highR3 + lowR3) - lowR4 - bandR4;
		bandR4 = f4R * highR4 + bandR4;

		*sp++ = clamp((highR4 + lowR4) * mixerGain, -ratioTimbres, ratioTimbres);
	}

	v0L = lowL;
	v1L = bandL;
	v0R = lowR;
	v1R = bandR;

	v2L = lowL2;
	v3L = bandL2;
	v2R = lowR2;
	v3R = bandR2;

	v4L = lowL3;
	v5L = bandL3;
	v4R = lowR3;
	v5R = bandR3;

	v6L = lowL4;
	v7L = bandL4;
	v6R = lowR4;
	v7R = bandR4;
}
break;
case FILTER_AP4 :
{
	//http://denniscronin.net/dsp/vst.html
	float fxParamTmp = (params.effect.param1 + matrixFilterFrequency);
	fxParamTmp *= fxParamTmp;
	fxParam1 = ((fxParamTmp + 9.0f * fxParam1) * .1f);

	float OffsetTmp = fabsf(params.effect.param2);
	fxParam2 = ((OffsetTmp + 9.0f * fxParam2) * .1f);

	const float bipolarf = (fxParam1 - 0.5f);

	const float folded = fold(sigmoid(bipolarf * 7 * fxParam2)) * 4; // -1 < folded < 1

	const float offset = fxParam2 * fxParam2 * 0.17f;
	const float lrDelta = 0.005f * folded;

	const float range = 0.47f;

	const float f1L = clamp(fold((fxParam1 - offset - lrDelta) * range) * 2, filterWindowMin, filterWindowMax);
	const float f2L = clamp(fold((fxParam1 + offset + lrDelta) * range) * 2, filterWindowMin, filterWindowMax);
	const float f3L = clamp(fold((fxParam1 - (offset * 2) - lrDelta) * range) * 2, filterWindowMin, filterWindowMax);
	const float f4L = clamp(fold((fxParam1 + (offset * 2) + lrDelta) * range) * 2, filterWindowMin, filterWindowMax);
	float coef1L = (1.0f - f1L) / (1.0f + f1L);
	float coef2L = (1.0f - f2L) / (1.0f + f2L);
	float coef3L = (1.0f - f3L) / (1.0f + f3L);
	float coef4L = (1.0f - f4L) / (1.0f + f4L);
	const float f1R = clamp(fold((f1L - lrDelta) * range) * 2, filterWindowMin, filterWindowMax);
	const float f2R = clamp(fold((f2L + lrDelta) * range) * 2, filterWindowMin, filterWindowMax);
	const float f3R = clamp(fold((f3L - lrDelta) * range) * 2, filterWindowMin, filterWindowMax);
	const float f4R = clamp(fold((f4L + lrDelta) * range) * 2, filterWindowMin, filterWindowMax);
	float coef1R = (1.0f - f1R) / (1.0f + f1R);
	float coef2R = (1.0f - f2R) / (1.0f + f2R);
	float coef3R = (1.0f - f3R) / (1.0f + f3R);
	float coef4R = (1.0f - f4R) / (1.0f + f4R);

	float *sp = this->sampleBlock;

	float inmix;
	float _ly1L = v0L, _ly1R = v0R;
	float _ly2L = v1L, _ly2R = v1R;
	float _ly3L = v2L, _ly3R = v2R;
	float _ly4L = v3L, _ly4R = v3R;
	float _lx1L = v4L, _lx1R = v4R;
	float _lx2L = v5L, _lx2R = v5R;
	float _lx3L = v6L, _lx3R = v6R;
	float _lx4L = v7L, _lx4R = v7R;
	float _feedback = 0.08f;
	float _crossFeedback = 0.16f;
	float finalGain = mixerGain * 0.5f;

	for (int k = BLOCK_SIZE ; k--; ) {
		// Left voice
		inmix = (*sp) + _ly3L * _feedback - _ly2R * _crossFeedback;

		_ly1L = coef1L * (_ly1L + inmix) - _lx1L; // do 1st filter
		_lx1L = inmix;
		_ly2L = coef2L * (_ly2L + _ly1L) - _lx2L; // do 2nd filter
		_lx2L = _ly1L;
		_ly3L = coef3L * (_ly3L + _ly2L) - _lx3L; // do 3rd filter
		_lx3L = _ly2L;
		_ly4L = coef4L * (_ly4L + _ly3L) - _lx4L; // do 4th filter
		_lx4L = _ly3L;

		*sp++ = clamp((*sp + _ly4L) * finalGain, -ratioTimbres, ratioTimbres);

		// Right voice
		inmix = (*sp) + _ly3R * _feedback - _ly2L * _crossFeedback;

		_ly1R = coef1R * (_ly1R + inmix) - _lx1R; // do 1st filter
		_lx1R = inmix;
		_ly2R = coef2R * (_ly2R + _ly1R) - _lx2R; // do 2nd filter
		_lx2R = _ly1R;
		_ly3R = coef3R * (_ly3R + _ly2R) - _lx3R; // do 3rd filter
		_lx3R = _ly2R;
		_ly4R = coef4R * (_ly4R + _ly3R) - _lx4R; // do 4th filter
		_lx4R = _ly3R;

		*sp++ = clamp((*sp + _ly4R) * finalGain, -ratioTimbres, ratioTimbres);
	}

	v0L = _ly1L; v0R = _ly1R;
	v1L = _ly2L; v1R = _ly2R;
	v2L = _ly3L; v2R = _ly3R;
	v3L = _ly4L; v3R = _ly4R;
	v4L = _lx1L; v4R = _lx1R;
	v5L = _lx2L; v5R = _lx2R;
	v6L = _lx3L; v6R = _lx3R;
	v7L = _lx4L; v7R = _lx4R;
}
break;
case FILTER_AP4B :
{
	float fxParamTmp = (params.effect.param1 + matrixFilterFrequency);
	fxParamTmp *= fxParamTmp;
	fxParam1 = ((fxParamTmp + 9.0f * fxParam1) * .1f);
	
	float OffsetTmp = fabsf(params.effect.param2);
	fxParam2 = ((OffsetTmp + 9.0f * fxParam2) * .1f);

	const float bipolarf = (fxParam1 - 0.5f);
	const float folded = fold(sigmoid(bipolarf * 19 * fxParam2)) * 4; // -1 < folded < 1

	const float offset = fxParam2 * fxParam2 * 0.17f;
	const float lrDelta = 0.005f * folded;

	const float range = 0.47f;

	const float f1L = clamp(fold((fxParam1 - offset - lrDelta) * range) * 2, filterWindowMin, filterWindowMax);
	const float f2L = clamp(fold((fxParam1 + offset + lrDelta) * range) * 2, filterWindowMin, filterWindowMax);
	const float f3L = clamp(fold((fxParam1 - (offset * 2) - lrDelta) * range) * 2, filterWindowMin, filterWindowMax);
	const float f4L = clamp(fold((fxParam1 + (offset * 2) + lrDelta) * range) * 2, filterWindowMin, filterWindowMax);
	float coef1L = (1.0f - f1L) / (1.0f + f1L);
	float coef2L = (1.0f - f2L) / (1.0f + f2L);
	float coef3L = (1.0f - f3L) / (1.0f + f3L);
	float coef4L = (1.0f - f4L) / (1.0f + f4L);
	const float f1R = clamp(fold((fxParam1 - offset + lrDelta) * range) * 2, filterWindowMin, filterWindowMax);
	const float f2R = clamp(fold((fxParam1 + offset - lrDelta) * range) * 2, filterWindowMin, filterWindowMax);
	const float f3R = clamp(fold((fxParam1 - (offset * 2) + lrDelta) * range) * 2, filterWindowMin, filterWindowMax);
	const float f4R = clamp(fold((fxParam1 + (offset * 2) - lrDelta) * range) * 2, filterWindowMin, filterWindowMax);
	float coef1R = (1.0f - f1R) / (1.0f + f1R);
	float coef2R = (1.0f - f2R) / (1.0f + f2R);
	float coef3R = (1.0f - f3R) / (1.0f + f3R);
	float coef4R = (1.0f - f4R) / (1.0f + f4R);

	float *sp = this->sampleBlock;

	float _ly1L = v0L, _ly1R = v0R;
	float _ly2L = v1L, _ly2R = v1R;
	float _ly3L = v2L, _ly3R = v2R;
	float _ly4L = v3L, _ly4R = v3R;
	float _lx1L = v4L, _lx1R = v4R;
	float _lx2L = v5L, _lx2R = v5R;
	float _lx3L = v6L, _lx3R = v6R;
	float _lx4L = v7L, _lx4R = v7R;

	float _feedback = 0.68f;
	float _crossFeedback = 0.17f;
	float inmix;
	float finalGain = mixerGain * 0.5f;

	for (int k = BLOCK_SIZE ; k--; ) {
		// Left voice
		inmix = (*sp) - _feedback * _ly3L + _crossFeedback * _ly3R;

		_ly1L = coef1L * (_ly1L + inmix) - _lx1L; // do 1st filter
		_lx1L = inmix;
		_ly2L = coef2L * (_ly2L + _ly1L) - _lx2L; // do 2nd filter
		_lx2L = _ly1L;
		_ly3L = coef3L * (_ly3L + _ly2L) - _lx3L; // do 3rd filter
		_lx3L = _ly2L;
		_ly4L = coef4L * (_ly4L + _ly3L) - _lx4L; // do 4nth filter
		_lx4L = _ly3L;

		*sp++ = clamp((*sp + _ly4L) * finalGain, -ratioTimbres, ratioTimbres);

		// Right voice
		inmix = (*sp) - _feedback * _ly3R + _crossFeedback * _ly3L;

		_ly1R = coef1R * (_ly1R + inmix) - _lx1R; // do 1st filter
		_lx1R = inmix;
		_ly2R = coef2R * (_ly2R + _ly1R) - _lx2R; // do 2nd filter
		_lx2R = _ly1R;
		_ly3R = coef3R * (_ly3R + _ly2R) - _lx3R; // do 3rd filter
		_lx3R = _ly2R;
		_ly4R = coef4R * (_ly4R + _ly3R) - _lx4R; // do 4nth filter
		_lx4R = _ly3R;

		*sp++ = clamp((*sp + _ly4R) * finalGain, -ratioTimbres, ratioTimbres);
	}

	v0L = _ly1L; v0R = _ly1R;
	v1L = _ly2L; v1R = _ly2R;
	v2L = _ly3L; v2R = _ly3R;
	v3L = _ly4L; v3R = _ly4R;
	v4L = _lx1L; v4R = _lx1R;
	v5L = _lx2L; v5R = _lx2R;
	v6L = _lx3L; v6R = _lx3R;
	v7L = _lx4L; v7R = _lx4R;
}
break;
case FILTER_AP4D :
{
	float fxParamTmp = (params.effect.param1 + matrixFilterFrequency);
	const uint_fast8_t random = (*(uint_fast8_t *)noise) & 0xff;
	float randomF = (float)random * 0.00390625f;
	if (random < 76) {
		fxParam1 += (randomF * 0.002f) - 0.001f;
	}
	fxParamTmp *= fxParamTmp;
	fxParam1 = ((fxParamTmp + 9.0f * fxParam1) * .1f);
	
	float OffsetTmp = fabsf(params.effect.param2);
	fxParam2 = ((OffsetTmp + 9.0f * fxParam2) * .1f);

	const float offset = fxParam2 * fxParam2 * 0.17f;

	const float range1 = 0.33f;
	const float range2 = 0.14f;
	const float range3 = 0.05f;

	const float lowlimit = 0.027f;

	const float f1 = clamp(((lowlimit + fxParam1 + offset) * range1), filterWindowMin, filterWindowMax);
	const float f2 = clamp(((lowlimit + fxParam1 - offset) * range2), filterWindowMin, filterWindowMax);
	const float f3 = clamp(((lowlimit + fxParam1 + offset) * range3), filterWindowMin, filterWindowMax);
	float coef1 = (1.0f - f1) / (1.0f + f1);
	float coef2 = (1.0f - f2) / (1.0f + f2);
	float coef3 = (1.0f - f3) / (1.0f + f3);


	float *sp = this->sampleBlock;

	float _ly1L = v0L, _ly1R = v0R;
	float _ly2L = v1L, _ly2R = v1R;
	float _ly3L = v2L, _ly3R = v2R;
	float _ly4L = v3L, _ly4R = v3R;
	float _lx1L = v4L, _lx1R = v4R;
	float _lx2L = v5L, _lx2R = v5R;
	float _lx3L = v6L, _lx3R = v6R;
	float _lx4L = v7L, _lx4R = v7R;

	float _feedback = 0.15f + (randomF * 0.01) - 0.005f;
	float inmix;
	float finalGain = mixerGain * 0.5f;

	const float a = 0.85f;
	const float b = 1.f - a;

	const float f = 0.33f + f1;

	coef3 = clamp( coef3 + (*sp * 0.0075f), 0, 1);//nested f xmod

	for (int k = BLOCK_SIZE ; k--; ) {
		// Left voice
		inmix = (*sp) + _feedback * _ly4L;
		_ly1L = coef1 * (_ly1L + inmix) - _lx1L; // do 1st filter
		_lx1L = inmix;
		_ly2L = coef2 * (_ly3L + _ly1L) - _lx2L; // do 2nd filter
		_lx2L = _ly1L;
		_ly3L = _lx4L * (_ly3L + _ly2L) - _lx3L; // nested filter
		_lx3L = _ly2L;

		_lx4L = (_lx4L * 31 + coef3) * 0.03125f;// xmod damp

		_ly4L = (_ly3L * b) + (_ly4L * a); // lowpass

		*sp++ = clamp((_ly1L + _ly2L - *sp) * finalGain, -ratioTimbres, ratioTimbres);

		// Right voice
		inmix = (*sp) + _feedback * _ly4R;
		_ly1R = coef1 * (_ly1R + inmix) - _lx1R; // do 1st filter
		_lx1R = inmix;
		_ly2R = coef2 * (_ly3R + _ly1R) - _lx2R; // do 2nd filter
		_lx2R = _ly1R;
		_ly3R = _lx4R * (_ly3R + _ly2R) - _lx3R; // nested filter
		_lx3R = _ly2R;

		_lx4R = (_lx4R * 31 + coef3) * 0.03125f;// xmod damp

		_ly4R = (_ly3R * b) + (_ly4R * a); // lowpass

		*sp++ = clamp((_ly1R + _ly2R - *sp) * finalGain, -ratioTimbres, ratioTimbres);
	}

	v0L = _ly1L; v0R = _ly1R;
	v1L = _ly2L; v1R = _ly2R;
	v2L = _ly3L; v2R = _ly3R;
	v3L = _ly4L; v3R = _ly4R;
	v4L = _lx1L; v4R = _lx1R;
	v5L = _lx2L; v5R = _lx2R;
	v6L = _lx3L; v6R = _lx3R;
	v7L = _lx4L; v7R = _lx4R;
}
break;
case FILTER_ORYX:
	{
	//https://www.musicdsp.org/en/latest/Filters/23-state-variable.html
	float fxParamTmp = params.effect.param1 + matrixFilterFrequency;
	fxParamTmp *= fxParamTmp;
	// Low pass... on the Frequency
	fxParamTmp = (fold((fxParamTmp - 0.5f) * 0.25f) + 0.25f) * 2;
	fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

	float param2Tmp = (params.effect.param2);
	fxParam2 = ((param2Tmp + 19.0f * fxParam2) * .05f);
	const float rootf = 0.011f + ((fxParam2 * fxParam2) - 0.005f) * 0.08f;
	const float frange = 0.14f;
	const float bipolarf = (fxParam1 - 0.5f);

	float f1 = rootf + sigmoidPos((fxParam1 > 0.5f) ? (1 - fxParam1) : fxParam1) * frange;
	/*//f1 = fold(sigmoid(((f1 - 0.5f) * (2/frange)) * 7 * fxParam2)) * 4; // -1 < folded < 1*/
	const float fb = 0.08f + fxParam1 * 0.01f + noise[0] * 0.002f;
	const float scale = sqrt3(fb);

	const float spread = 1 + (fxParam2 * 0.13f);

	const float fold2 = fold(0.125f + sigmoid(bipolarf * 21 * fxParam2) * frange ) + 0.25f;
	const float f2 = rootf + frange * 1.15f + sigmoidPos(1 - fxParam1 + fold2 * 0.25f) * spread * frange * 1.25f;
	const float fb2 = 0.21f - fold2 * 0.08f - noise[1] * 0.0015f;
	const float scale2 = sqrt3(fb2);

	float *sp = this->sampleBlock;
	float lowL = v0L, bandL = v1L;
	float lowR = v0R, bandR = v1R;

	float lowL2 = v2L, bandL2 = v3L;
	float lowR2 = v2R, bandR2 = v3R;

	float lowL3 = v4L, bandL3 = v5L;
	float lowR3 = v4R, bandR3 = v5R;

	const float svfGain = (1 + SVFGAINOFFSET) * mixerGain;
	
	const float fap1 = clamp(f1, filterWindowMin, filterWindowMax);
	float coef1 = (1.0f - fap1) / (1.0f + fap1);

	float out;
	const float r = 0.985f;
	float nz;
	float drift = v6L;
	float nexDrift = noise[7] * 0.02f;
	float deltaD = (nexDrift - drift) * 0.000625f;

	const float sat = 1.55f;

	for (int k=BLOCK_SIZE ; k--; ) {
		nz = noise[k] * 0.005f;

		// ----------- Left voice
		*sp = (lowL + ((-lowL + *sp) * (r + nz)));
		// bandpass 1
		lowL += f1 * bandL;
		bandL += f1 * (scale * *sp - lowL - (fb + nz - drift) * bandL);

		// bandpass 2
		lowL2 += f2 * bandL2;
		bandL2 += f2 * (scale2 * *sp - lowL2 - fb2 * bandL2);

		out = sat25((bandL * sat)) + bandL2;

		*sp++ = clamp(out * svfGain, -ratioTimbres, ratioTimbres);

		// ----------- Right voice
		*sp = (lowR + ((-lowR + *sp) * (r - nz)));
		// bandpass 1
		lowR += f1 * bandR;
		bandR += f1 * (scale * *sp - lowR - (fb - nz + drift) * bandR);

		// bandpass 2
		lowR2 += f2 * bandR2;
		bandR2 += f2 * (scale2 * *sp - lowR2 - fb2 * bandR2);

		out = sat25((bandR * sat)) + bandR2;

		*sp++ = clamp(out * svfGain, -ratioTimbres, ratioTimbres);

		drift += deltaD;
	}

	v0L = lowL;
	v1L = bandL;
	v0R = lowR;
	v1R = bandR;
	
	v2L = lowL2;
	v3L = bandL2;
	v2R = lowR2;
	v3R = bandR2;

	v4L = lowL3;
	v5L = bandL3;
	v4R = lowR3;
	v5R = bandR3;

	v6L = nexDrift;
}
break;
case FILTER_ORYX2:
	{
	//https://www.musicdsp.org/en/latest/Filters/23-state-variable.html
	float fxParamTmp = params.effect.param1 + matrixFilterFrequency;
	fxParamTmp *= fxParamTmp;
	// Low pass... on the Frequency
	fxParamTmp = (fold((fxParamTmp - 0.5f) * 0.25f) + 0.25f) * 2;
	fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

	float param2Tmp = (params.effect.param2);
	fxParam2 = ((param2Tmp + 19.0f * fxParam2) * .05f);
	const float rootf = 0.031f + (fxParam2) * 0.0465f;
	const float frange = 0.22f;
	const float bipolarf = sigmoid(fxParam1 - 0.5f);

	const float f1 = rootf + sigmoidPos((fxParam1 > 0.55f) ? (0.7f - (fxParam1 * 0.7f)) : (fxParam1 * fxParam1)) * frange;
	const float fb = 0.074f + fxParam1 * 0.01f + noise[0] * 0.002f;
	const float scale = sqrt3(fb);

	const float spread = 1 + (fxParam2 * 0.13f);

	const float fold2 = fold(0.125f + sigmoid(bipolarf * 21 * fxParam2) * frange ) + 0.25f;
	const float f2 = rootf + frange * 1.1f - (sigmoidPos(fxParam1 + fold2 * 0.25f) - 0.5f) * spread * frange * 0.5f;
	const float fb2 = 0.11f - f1 * fold2 * 0.4f - noise[1] * 0.0015f;
	const float scale2 = sqrt3(fb2);

	const float fold3 = fold(sigmoid(bipolarf * 17 * fxParam2) * frange);
	const float f3 = rootf + frange * 1.35f - tanh3(((1 - fxParam1) * 2 - fold3 * 0.2f)) * spread * frange * 0.25f;
	const float fb3 = 0.13f - f1 * fold3 * 0.5f - noise[2] * 0.001f;
	const float scale3 = sqrt3(fb3);

	float *sp = this->sampleBlock;
	float lowL = v0L, bandL = v1L;
	float lowR = v0R, bandR = v1R;

	float lowL2 = v2L, bandL2 = v3L;
	float lowR2 = v2R, bandR2 = v3R;

	float lowL3 = v4L, bandL3 = v5L;
	float lowR3 = v4R, bandR3 = v5R;

	const float svfGain = (1 + SVFGAINOFFSET) * mixerGain;
	
	const float fap1 = clamp(f1, filterWindowMin, filterWindowMax);
	float coef1 = (1.0f - fap1) / (1.0f + fap1);

	float out;
	const float r = 0.985f;
	float nz;
	float drift = v6L;
	float nexDrift = noise[7] * 0.02f;
	float deltaD = (nexDrift - drift) * 0.000625f;

	for (int k=BLOCK_SIZE ; k--; ) {
		nz = noise[k] * 0.016f;

		// ----------- Left voice
		*sp = lowL + ((-lowL + *sp) * (r + nz));
		// bandpass 1
		lowL += f1 * bandL;
		bandL += f1 * (scale * *sp - lowL - (fb + nz - drift) * bandL);

		// bandpass 2
		lowL2 += (f2 + drift) * bandL2;
		bandL2 += f2 * (scale2 * *sp - lowL2 - fb2 * bandL2);

		// bandpass 3
		lowL3 += f3 * bandL3;
		bandL3 += f3 * (scale3 * *sp - lowL3 - fb3 * bandL3);

		out = (bandL) + (bandL2 + bandL3);

		*sp++ = clamp(out * svfGain, -ratioTimbres, ratioTimbres);

		// ----------- Right voice
		*sp = lowR + ((-lowR + *sp) * (r - nz));
		// bandpass 1
		lowR += f1 * bandR;
		bandR += f1 * (scale * *sp - lowR - (fb - nz + drift) * bandR);

		// bandpass 2
		lowR2 += (f2 + drift) * bandR2;
		bandR2 += f2 * (scale2 * *sp - lowR2 - fb2 * bandR2);

		// bandpass 3
		lowR3 += f3 * bandR3;
		bandR3 += f3 * (scale3 * *sp - lowR3 - fb3 * bandR3);

		out = (bandR) + (bandR2 + bandR3);

		*sp++ = clamp(out * svfGain, -ratioTimbres, ratioTimbres);

		drift += deltaD;
	}

	v0L = lowL;
	v1L = bandL;
	v0R = lowR;
	v1R = bandR;
	
	v2L = lowL2;
	v3L = bandL2;
	v2R = lowR2;
	v3R = bandR2;

	v4L = lowL3;
	v5L = bandL3;
	v4R = lowR3;
	v5R = bandR3;

	v6L = nexDrift;
}
break;
case FILTER_ORYX3:
{
	//https://www.musicdsp.org/en/latest/Filters/23-state-variable.html
	float fxParamTmp = params.effect.param1 + matrixFilterFrequency;
	fxParamTmp *= fxParamTmp;
	// Low pass... on the Frequency
	fxParamTmp = (fold((fxParamTmp - 0.5f) * 0.25f) + 0.25f) * 2;
	fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

	float param2Tmp = (params.effect.param2);
	fxParam2 = ((param2Tmp + 19.0f * fxParam2) * .05f);
	const float rootf = 0.011f + ((fxParam2 * fxParam2) - 0.005f) * 0.08f;
	const float frange = 0.14f;
	const float bipolarf = (fxParam1 - 0.5f);

	const float f1 = rootf + sigmoidPos((fxParam1 > 0.5f) ? (1 - fxParam1) : fxParam1) * frange;
	const float fb = 0.082f + fxParam1 * 0.01f + noise[0] * 0.002f;
	const float scale = sqrt3(fb);

	const float spread = 1 + (fxParam2 * 0.13f);

	const float fold2 = fold(0.125f + sigmoid(bipolarf * 21 * fxParam2) * frange ) + 0.25f;
	const float f2 = rootf + frange * 1.15f + sigmoidPos(1 - fxParam1 + fold2 * 0.25f) * spread * frange * 1.25f;
	const float fb2 = 0.11f - fold2 * 0.04f - noise[1] * 0.0015f;
	const float scale2 = sqrt3(fb2);

	const float fold3 = fold(sigmoid(bipolarf * 17 * fxParam2) * frange);
	const float f3 = rootf + frange * 2.25f + tanh3(1 - (fxParam1 * 2 - fold3 * 0.2f)) * spread * frange * 0.5f;
	const float fb3 = 0.13f - fold3 * 0.05f - noise[2] * 0.001f;
	const float scale3 = sqrt3(fb3);

	float *sp = this->sampleBlock;
	float low = v0L, band = v1L;
	float low2 = v2L, band2 = v3L;
	float low3 = v4L, band3 = v5L;
	float drift = v6L;

	float _ly1L = v0R, _lx1L = v1R;
	float _ly1R = v2R, _lx1R = v3R;

	float _ly2L = v4R, _lx2L = v5R;
	float _ly2R = v6R, _lx2R = v7R;

	const float svfGain = (1 + SVFGAINOFFSET) * mixerGain;

	float in, out;
	const float r = 0.985f;
	float nz;
	float nexDrift = noise[7] * 0.02f;
	float deltaD = (nexDrift - drift) * 0.000625f;

	const float apf1 = clamp(0.33f + fxParam2 * fxParam2 * 0.36f , filterWindowMin, filterWindowMax);
	float coef1 = (1.0f - apf1) / (1.0f + apf1);
	const float apf2 = clamp(0.3f + fxParam2 * fxParam2 * 0.4f , filterWindowMin, filterWindowMax);
	float coef2 = (1.0f - apf2) / (1.0f + apf2);

	float sat = 1.65f;
	float sat2 = 1.45f;

	for (int k=BLOCK_SIZE ; k--; ) {
		nz = noise[k] * 0.013f;

		in = (*sp + *(sp + 1)) * 0.5f;

		in = low + ((-low + in) * (r + nz));
		// bandpass 1
		low += f1 * band;
		band += f1 * (scale * in - low - (fb + nz - drift) * band);

		// bandpass 2
		low2 += f2 * band2;
		band2 += f2 * (scale2 * in - low2 - fb2 * band2);

		// bandpass 3
		low3 += f3 * band3;
		band3 += f3 * (scale3 * in - low3 - fb3 * band3);

		out = sat25(band * sat) + tanh4((band2 * sat) + band3  * 0.75f) * 0.5f;

		// ----------- Left voice
		_ly1L = coef1 * (_ly1L + out) - _lx1L; // allpass
		_lx1L = out;

		*sp++ = clamp(_ly1L * svfGain, -ratioTimbres, ratioTimbres);

		// ----------- Right voice
		_ly2L = coef2 * (_ly2L + out) - _lx2L; // allpass
		_lx2L = out;

		*sp++ = clamp(_ly2L * svfGain, -ratioTimbres, ratioTimbres);

		drift += deltaD;
	}

	v0L = low;
	v1L = band;
	v2L = low2;
	v3L = band2;
	v4L = low3;
	v5L = band3;
	v6L = nexDrift;

	v0R = _ly1L;
	v1R = _lx1L;
	v2R = _ly1R;
	v3R = _lx1R;
	
	v4R = _ly2L;
	v5R = _lx2L;
	v6R = _ly2R;
	v7R = _lx2R;

	break;
}
case FILTER_18DB:
{
	//https://www.musicdsp.org/en/latest/Filters/26-moog-vcf-variation-2.html
	float fxParamTmp = (params.effect.param1 + matrixFilterFrequency);
	fxParamTmp *= fxParamTmp;

	fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0 , 1);
	float cut = clamp(sqrt3(fxParam1), filterWindowMin, 1);
	float cut2 = cut * cut;
	float attn = 0.37013f * cut2 * cut2;
	float inAtn = 0.3f;


	float OffsetTmp = fabsf(params.effect.param2);
	fxParam2 = ((OffsetTmp + 9.0f * fxParam2) * .1f);
	float res = fxParam2 * 6 + cut2 * 0.68f;
 	float fb = res * (1 - 0.178f * cut2);
	float invCut = 1 - cut;

	//const float r = 0.985f;
	const float bipolarf = fxParam1 - 0.5f;
	const float fold3 = (fold(13 * bipolarf) + 0.125f) * 1.5f;
	float dc  = cut2 * 0.1f;
	float *sp = this->sampleBlock;

	float buf1L = v0L, buf2L = v1L, buf3L = v2L, buf4L = v3L;
	float buf1R = v0R, buf2R = v1R, buf3R = v2R, buf4R = v3R;

	float buf1inL = v4L, buf2inL = v5L, buf3inL = v6L, buf4inL = v7L;
	float buf1inR = v4R, buf2inR = v5R, buf3inR = v6R, buf4inR = v7R;

	float finalGain = mixerGain * (1 + fxParam2 * 0.75f) * 2;

	const float r = 0.987f;

	for (int k = BLOCK_SIZE ; k--; ) {

		// Left voice
		*sp = buf3L + ((-buf3L + *sp) * r);

		*sp = tanh3(*sp - (buf3L) * (fb));
		*sp *= attn;
		buf1L = (*sp) + inAtn * buf1inL + invCut * buf1L; // Pole 1
		buf1inL = (*sp);
		buf2L = (buf1L) + inAtn * buf2inL + invCut * buf2L; // Pole 2
		buf2inL = buf1L;
		buf3L = (buf2L) + inAtn * buf3inL + invCut * buf3L; // Pole 3
		buf3inL = buf2L;

		*sp++ = clamp(buf3L * finalGain, -ratioTimbres, ratioTimbres);

		// Right voice
		*sp = buf3R + ((-buf3R + *sp) * r);

		*sp = tanh3(*sp - (buf3R) * (fb));
		*sp *= attn;
		buf1R = (*sp) + inAtn * buf1inR + invCut * buf1R; // Pole 1
		buf1inR = (*sp);
		buf2R = (buf1R) + inAtn * buf2inR + invCut * buf2R; // Pole 2
		buf2inR = buf1R;
		buf3R = (buf2R) + inAtn * buf3inR + invCut * buf3R; // Pole 3
		buf3inR = buf2R;

		*sp++ = clamp(buf3R * finalGain, -ratioTimbres, ratioTimbres);
	}

	v0L = buf1L;
	v1L = buf2L;
	v2L = buf3L;
	v3L = buf4L;

	v0R = buf1R;
	v1R = buf2R;
	v2R = buf3R;
	v3R = buf4R;

	v4L = buf1inL;
	v5L = buf2inL;
	v6L = buf3inL;
	v7L = buf4inL;

	v4R = buf1inR;
	v5R = buf2inR;
	v6R = buf3inR;
	v7R = buf4inR;

	break;
}
break;
case FILTER_LADDER:
{
	//https://www.musicdsp.org/en/latest/Filters/26-moog-vcf-variation-2.html
	float fxParamTmp = (params.effect.param1 + matrixFilterFrequency);
	fxParamTmp *= fxParamTmp;

	fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0 , 1);
	float cut = clamp(sqrt3(fxParam1), filterWindowMin, 1);
	float cut2 = cut * cut;
	float attn = 0.35013f * cut2 * cut2;
	float inAtn = 0.3f;

	float OffsetTmp = fabsf(params.effect.param2);
	fxParam2 = ((OffsetTmp + 9.0f * fxParam2) * .1f);
	float res = fxParam2 * 4.5f * (0.8f + cut2 * 0.2f);
 	float fb = res * (1 - 0.15f * cut2);
	float invCut = 1 - cut;

	const float r = 0.985f;

	float *sp = this->sampleBlock;

	float buf1L = v0L, buf2L = v1L, buf3L = v2L, buf4L = v3L;
	float buf1R = v0R, buf2R = v1R, buf3R = v2R, buf4R = v3R;

	float buf1inL = v4L, buf2inL = v5L, buf3inL = v6L, buf4inL = v7L;
	float buf1inR = v4R, buf2inR = v5R, buf3inR = v6R, buf4inR = v7R;

	float finalGain = mixerGain * (1 + fxParam2 * 2);

	for (int k = BLOCK_SIZE ; k--; ) {

		// Left voice
		*sp = buf4L + ((-buf4L + *sp) * r);

		*sp -= tanh3(0.005f + buf4L) * (fb + buf4R * 0.05f);
		*sp *= attn;
		buf1L = *sp + inAtn * buf1inL + invCut * buf1L; // Pole 1
		buf1inL = *sp;
		buf2L = buf1L + inAtn * buf2inL + invCut * buf2L; // Pole 2
		buf2inL = buf1L;
		buf3L = buf2L + inAtn * buf3inL + invCut * buf3L; // Pole 3
		buf3inL = buf2L;
		buf4L = buf3L + inAtn * buf4inL + invCut * buf4L; // Pole 4
		buf4inL = buf3L;

		*sp++ = clamp(buf4L * finalGain, -ratioTimbres, ratioTimbres);

		// Right voice
		*sp = buf4R + ((-buf4R + *sp) * r);

		*sp -= tanh3(0.005f + buf4R) * (fb - buf4L * 0.05f);
		*sp *= attn;
		buf1R = *sp + inAtn * buf1inR + invCut * buf1R; // Pole 1
		buf1inR = *sp;
		buf2R = buf1R + inAtn * buf2inR + invCut * buf2R; // Pole 2
		buf2inR = buf1R;
		buf3R = buf2R + inAtn * buf3inR + invCut * buf3R; // Pole 3
		buf3inR = buf2R;
		buf4R = buf3R + inAtn * buf4inR + invCut * buf4R; // Pole 4
		buf4inR = buf3R;

		*sp++ = clamp(buf4R * finalGain, -ratioTimbres, ratioTimbres);
	}

	v0L = buf1L;
	v1L = buf2L;
	v2L = buf3L;
	v3L = buf4L;

	v0R = buf1R;
	v1R = buf2R;
	v2R = buf3R;
	v3R = buf4R;

	v4L = buf1inL;
	v5L = buf2inL;
	v6L = buf3inL;
	v7L = buf4inL;

	v4R = buf1inR;
	v5R = buf2inR;
	v6R = buf3inR;
	v7R = buf4inR;

	break;
}
case FILTER_LADDER2:
{
	//https://www.musicdsp.org/en/latest/Filters/240-karlsen-fast-ladder.html
	float fxParamTmp = (params.effect.param1 + matrixFilterFrequency);
	fxParamTmp *= fxParamTmp;

	fxParam1 = ((fxParamTmp + 9.0f * fxParam1) * .1f);
	float cut = clamp(sqrt3(fxParam1), filterWindowMin, filterWindowMax);

	float OffsetTmp = fabsf(params.effect.param2);
	fxParam2 = ((OffsetTmp + 9.0f * fxParam2) * .1f);
	float res = fxParam2;

	float *sp = this->sampleBlock;

	float buf1L = v0L, buf2L = v1L, buf3L = v2L, buf4L = v3L;
	float buf1R = v0R, buf2R = v1R, buf3R = v2R, buf4R = v3R;

	float _ly1L = v4L, _lx1L = v5L;
	float _ly1R = v4R, _lx1R = v5R;

	float _ly2L = v6L, _ly2R = v6R;
	float _lx2L = v7L, _lx2R = v7R;

	float resoclip;

	const float bipolarf = fxParam1 - 0.5f;
	const float fold3 = (fold(13 * bipolarf) + 0.125f) * 1.5f;
	const float f1 = clamp(0.33f + fold3 * (1 - cut * 0.5f) , filterWindowMin, filterWindowMax);
	float coef1 = (1.0f - f1) / (1.0f + f1);
	const float f2 = clamp(f1 * f1, filterWindowMin, filterWindowMax);
	float coef2 = (1.0f - f2) / (1.0f + f2);

	float finalGain = mixerGain * 1.25f;
	float nz;

	for (int k = BLOCK_SIZE ; k--; ) {

		// Left voice
		*sp = *sp - sat25(0.025f + buf4L * (res + buf4R * 0.05f));
		buf1L = ((*sp - buf1L) * cut) + buf1L;
		buf2L = ((buf1L - buf2L) * cut) + buf2L;
		_ly1L = coef1 * (_ly1L + buf2L) - _lx1L; // allpass
		_lx1L = buf2L;
		buf3L = ((_ly1L - buf3L) * cut) + buf3L;
		_ly2L = coef2 * (_ly2L + buf3L) - _lx2L; // allpass
		_lx2L = buf3L;
		buf4L = ((_ly2L - buf4L) * cut) + buf4L;
		*sp++ = clamp(buf4L * finalGain, -ratioTimbres, ratioTimbres);

		// Right voice
		*sp = *sp - sat25(0.025f + buf4R * (res - buf4L * 0.05f));
		buf1R = ((*sp - buf1R) * cut) + buf1R;
		buf2R = ((buf1R - buf2R) * cut) + buf2R;
		_ly1R = coef1 * (_ly1R + buf2R) - _lx1R; // allpass
		_lx1R = buf2R;
		buf3R = ((_ly1R - buf3R) * cut) + buf3R;
		_ly2R = coef2 * (_ly2R + buf3R) - _lx2R; // allpass
		_lx2R = buf3R;
		buf4R = ((_ly2R - buf4R) * cut) + buf4R;
		*sp++ = clamp(buf4R * finalGain, -ratioTimbres, ratioTimbres);
	}

	v0L = buf1L;
	v1L = buf2L;
	v2L = buf3L;
	v3L = buf4L;

	v0R = buf1R;
	v1R = buf2R;
	v2R = buf3R;
	v3R = buf4R;

	v4L = _ly1L;
	v5L = _lx1L;
	v4R = _ly1R;
	v5R = _lx1R;

	v6L = _ly2L;
	v6R = _ly2R;
	v7L = _lx2L;
	v7R = _lx2R;

	break;
}
case FILTER_DIOD:
{
	float fxParamTmp = SVFOFFSET + params.effect.param1 + matrixFilterFrequency;
	fxParamTmp *= fxParamTmp;
	// Low pass... on the Frequency
	fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

	const float f = fxParam1 * fxParam1;
	const float fmeta = f * sigmoid(f);
	const float fb = sqrt3(0.7f - fxParam2 * 0.699f);
	const float scale = sqrt3(fb);

	float dc  = f * 0.05f * fb;
	const float f2 = 0.25f + f * 0.5f;

	float *sp = this->sampleBlock;
	float lowL = v0L, highL = 0, bandL = v1L;
	float lowR = v0R, highR = 0, bandR = v1R;

	const float svfGain = (1 + fxParam2 * fxParam2 * 0.75f) * mixerGain;

	float _ly1L = v2L, _ly1R = v2R;
	float _lx1L = v3L, _lx1R = v3R;
	const float f1 = clamp(0.33f + f * 0.43f, filterWindowMin, filterWindowMax);
	float coef1 = (1.0f - f1) / (1.0f + f1);

	float attn = 0.05f + 0.2f * f * f * f;
	float r = 0.985f;
	float hibp;

	for (int k = BLOCK_SIZE; k--;)	{
		// Left voice
		*sp = (lowL + ((-lowL + *sp) * r));

		hibp = attn * clamp(*sp - bandL, -ratioTimbres, ratioTimbres);

		lowL += fmeta * hibp;
		bandL += fmeta * (scale * *sp - lowL - fb * (hibp));

		lowL += f * bandL;
		bandL += f * (scale * (*sp + dc) - lowL - fb * (bandL));

		*sp++ = clamp(lowL * svfGain, -ratioTimbres, ratioTimbres);

		// Right voice
		*sp = (lowR + ((-lowR + *sp) * r));

		hibp = attn * clamp(*sp - bandR, -ratioTimbres, ratioTimbres);

		lowR += fmeta * hibp;
		bandR += fmeta * (scale * *sp - lowR - fb * (hibp));

		lowR += f * bandR;
		bandR += f * (scale * (*sp + dc) - lowR - fb * (bandR));

		*sp++ = clamp(lowR * svfGain, -ratioTimbres, ratioTimbres);
	}

	v0L = lowL;
	v1L = bandL;
	v0R = lowR;
	v1R = bandR;

	v2L = _ly1L; v2R = _ly1R;
	v3L = _lx1L; v3R = _lx1R;
}
break;
case FILTER_KRMG:
{
	//https://github.com/ddiakopoulos/MoogLadders/blob/master/src/KrajeskiModel.h
	float fxParamTmp = SVFOFFSET + params.effect.param1 + matrixFilterFrequency;
	fxParamTmp *= fxParamTmp;
	// Low pass... on the Frequency
	fxParam1 = clamp((fxParamTmp + 9.0f * fxParam1) * .1f, 0, 1);

	const float wc = fxParam1 * fxParam1;
	const float wc2 = wc * wc;
	const float wc3 = wc2 * wc;

	const float g = 0.9892f * wc - 0.4342f * wc2 + 0.1381f * wc3 - 0.0202f * wc2 * wc2;
	const float drive = 1;
	const float gComp = 1;
	const float resonance = fxParam2 * 0.72f - fxParam1 * 0.05f;
	const float gRes = 4 * resonance * (1.0029f + 0.0526f * wc - 0.926f * wc2 + 0.0218f * wc3);

	float *sp = this->sampleBlock;
	float state0L = v0L, state1L = v1L, state2L = v2L, state3L = v3L, state4L = v4L;
	float delay0L = v5L, delay1L = v6L, delay2L = v7L, delay3L = v8L;

	float state0R = v0R, state1R = v1R, state2R = v2R, state3R = v3R, state4R = v4R;
	float delay0R = v5R, delay1R = v6R, delay2R = v7R, delay3R = v8R;

	const float va1 = 0.2307692308f;//0.3f / 1.3f;
	const float va2 = 0.7692307692f;//1 / 1.3f;
	float r = 0.989f;

	float drift = fxParamB2;
	float nz;
	float nexDrift = noise[7] * 0.015f;
	float deltaD = (nexDrift - drift) * 0.000625f;

	for (int k = BLOCK_SIZE; k--;)
	{
		nz = noise[k] * 0.012f;
		// Left voice
		*sp = (state4L + ((-state4L + *sp) * r));
		state0L = tanh4((drive - drift  + nz) * (*sp - gRes * (state4L - gComp * *sp)));

		state1L = g * (va1 * state0L + va2 * delay0L - state1L) + state1L;
		delay0L = state0L;

		state2L = g * (va1 * state1L + va2 * delay1L - state2L) + state2L;
		delay1L = state1L;

		state3L = g * (va1 * state2L + va2 * delay2L - state3L) + state3L;
		delay2L = state2L;

		state4L = g * (va1 * state3L + va2 * delay3L - state4L) + state4L;
		delay3L = state3L;

		*sp++ = clamp(state4L * mixerGain, -ratioTimbres, ratioTimbres);

		// Right voice
		*sp = (state4R + ((-state4R + *sp) * r));
		state0R = tanh4((drive + drift - nz) * (*sp - gRes * (state4R - gComp * *sp)));

		state1R = g * (va1 * state0R + va2 * delay0R - state1R) + state1R;
		delay0R = state0R;

		state2R = g * (va1 * state1R + va2 * delay1R - state2R) + state2R;
		delay1R = state1R;

		state3R = g * (va1 * state2R + va2 * delay2R - state3R) + state3R;
		delay2R = state2R;

		state4R = g * (va1 * state3R + va2 * delay3R - state4R) + state4R;
		delay3R = state3R;

		*sp++ = clamp(state4R * mixerGain, -ratioTimbres, ratioTimbres);

		drift += deltaD;
	}

	v0L = state0L;
	v1L = state1L;
	v2L = state2L;
	v3L = state3L;
	v4L = state4L;

	v5L = delay0L;
	v6L = delay1L;
	v7L = delay2L;
	v8L = delay3L;

	v0R = state0R;
	v1R = state1R;
	v2R = state2R;
	v3R = state3R;
	v4R = state4R;

	v5R = delay0R;
	v6R = delay1R;
	v7R = delay2R;
	v8R = delay3R;

	fxParamB2 = nexDrift;
}
break;
/* case FILTER_TEEBEE:
{
const float filterpoles[10][64] = {
{-1.42475857e-02, -1.10558351e-02, -9.58097367e-03,
	-8.63568249e-03, -7.94942757e-03, -7.41570560e-03,
	-6.98187179e-03, -6.61819537e-03, -6.30631927e-03,
	-6.03415378e-03, -5.79333654e-03, -5.57785533e-03,
	-5.38325013e-03, -5.20612558e-03, -5.04383985e-03,
	-4.89429884e-03, -4.75581571e-03, -4.62701254e-03,
	-4.50674977e-03, -4.39407460e-03, -4.28818259e-03,
	-4.18838855e-03, -4.09410427e-03, -4.00482112e-03,
	-3.92009643e-03, -3.83954259e-03, -3.76281836e-03,
	-3.68962181e-03, -3.61968451e-03, -3.55276681e-03,
	-3.48865386e-03, -3.42715236e-03, -3.36808777e-03,
	-3.31130196e-03, -3.25665127e-03, -3.20400476e-03,
	-3.15324279e-03, -3.10425577e-03, -3.05694308e-03,
	-3.01121207e-03, -2.96697733e-03, -2.92415989e-03,
	-2.88268665e-03, -2.84248977e-03, -2.80350622e-03,
	-2.76567732e-03, -2.72894836e-03, -2.69326825e-03,
	-2.65858922e-03, -2.62486654e-03, -2.59205824e-03,
	-2.56012496e-03, -2.52902967e-03, -2.49873752e-03,
	-2.46921570e-03, -2.44043324e-03, -2.41236091e-03,
	-2.38497108e-03, -2.35823762e-03, -2.33213577e-03,
	-2.30664208e-03, -2.28173430e-03, -2.25739130e-03,
	-2.23359302e-03},
{1.63323670e-16, -1.61447133e-02, -1.99932070e-02,
	-2.09872000e-02, -2.09377795e-02, -2.04470150e-02,
	-1.97637613e-02, -1.90036975e-02, -1.82242987e-02,
	-1.74550383e-02, -1.67110053e-02, -1.59995606e-02,
	-1.53237941e-02, -1.46844019e-02, -1.40807436e-02,
	-1.35114504e-02, -1.29747831e-02, -1.24688429e-02,
	-1.19916965e-02, -1.15414484e-02, -1.11162818e-02,
	-1.07144801e-02, -1.03344362e-02, -9.97465446e-03,
	-9.63374867e-03, -9.31043725e-03, -9.00353710e-03,
	-8.71195702e-03, -8.43469084e-03, -8.17081077e-03,
	-7.91946102e-03, -7.67985179e-03, -7.45125367e-03,
	-7.23299254e-03, -7.02444481e-03, -6.82503313e-03,
	-6.63422244e-03, -6.45151640e-03, -6.27645413e-03,
	-6.10860728e-03, -5.94757730e-03, -5.79299303e-03,
	-5.64450848e-03, -5.50180082e-03, -5.36456851e-03,
	-5.23252970e-03, -5.10542063e-03, -4.98299431e-03,
	-4.86501921e-03, -4.75127814e-03, -4.64156716e-03,
	-4.53569463e-03, -4.43348032e-03, -4.33475462e-03,
	-4.23935774e-03, -4.14713908e-03, -4.05795659e-03,
	-3.97167614e-03, -3.88817107e-03, -3.80732162e-03,
	-3.72901453e-03, -3.65314257e-03, -3.57960420e-03,
	-3.50830319e-03},
{-1.83545593e-06, -1.35008051e-03, -1.51527847e-03,
	-1.61437715e-03, -1.68536679e-03, -1.74064961e-03,
	-1.78587681e-03, -1.82410854e-03, -1.85719118e-03,
	-1.88632533e-03, -1.91233586e-03, -1.93581405e-03,
	-1.95719818e-03, -1.97682215e-03, -1.99494618e-03,
	-2.01177700e-03, -2.02748155e-03, -2.04219657e-03,
	-2.05603546e-03, -2.06909331e-03, -2.08145062e-03,
	-2.09317612e-03, -2.10432901e-03, -2.11496056e-03,
	-2.12511553e-03, -2.13483321e-03, -2.14414822e-03,
	-2.15309131e-03, -2.16168985e-03, -2.16996830e-03,
	-2.17794867e-03, -2.18565078e-03, -2.19309254e-03,
	-2.20029023e-03, -2.20725864e-03, -2.21401130e-03,
	-2.22056055e-03, -2.22691775e-03, -2.23309332e-03,
	-2.23909688e-03, -2.24493730e-03, -2.25062280e-03,
	-2.25616099e-03, -2.26155896e-03, -2.26682328e-03,
	-2.27196010e-03, -2.27697514e-03, -2.28187376e-03,
	-2.28666097e-03, -2.29134148e-03, -2.29591970e-03,
	-2.30039977e-03, -2.30478562e-03, -2.30908091e-03,
	-2.31328911e-03, -2.31741351e-03, -2.32145721e-03,
	-2.32542313e-03, -2.32931406e-03, -2.33313263e-03,
	-2.33688133e-03, -2.34056255e-03, -2.34417854e-03,
	-2.34773145e-03},
{-2.96292613e-06, 6.75138822e-04, 6.96581050e-04,
	7.04457808e-04, 7.07837502e-04, 7.09169651e-04,
	7.09415480e-04, 7.09031433e-04, 7.08261454e-04,
	7.07246872e-04, 7.06074484e-04, 7.04799978e-04,
	7.03460301e-04, 7.02080606e-04, 7.00678368e-04,
	6.99265907e-04, 6.97852005e-04, 6.96442963e-04,
	6.95043317e-04, 6.93656323e-04, 6.92284301e-04,
	6.90928882e-04, 6.89591181e-04, 6.88271928e-04,
	6.86971561e-04, 6.85690300e-04, 6.84428197e-04,
	6.83185182e-04, 6.81961088e-04, 6.80755680e-04,
	6.79568668e-04, 6.78399727e-04, 6.77248505e-04,
	6.76114631e-04, 6.74997722e-04, 6.73897392e-04,
	6.72813249e-04, 6.71744904e-04, 6.70691972e-04,
	6.69654071e-04, 6.68630828e-04, 6.67621875e-04,
	6.66626854e-04, 6.65645417e-04, 6.64677222e-04,
	6.63721940e-04, 6.62779248e-04, 6.61848835e-04,
	6.60930398e-04, 6.60023644e-04, 6.59128290e-04,
	6.58244058e-04, 6.57370684e-04, 6.56507909e-04,
	6.55655483e-04, 6.54813164e-04, 6.53980718e-04,
	6.53157918e-04, 6.52344545e-04, 6.51540387e-04,
	6.50745236e-04, 6.49958895e-04, 6.49181169e-04,
	6.48411873e-04},
{-1.00014774e+00, -1.35336624e+00, -1.42048887e+00,
	-1.46551548e+00, -1.50035433e+00, -1.52916086e+00,
	-1.55392254e+00, -1.57575858e+00, -1.59536715e+00,
	-1.61321568e+00, -1.62963377e+00, -1.64486333e+00,
	-1.65908760e+00, -1.67244897e+00, -1.68506052e+00,
	-1.69701363e+00, -1.70838333e+00, -1.71923202e+00,
	-1.72961221e+00, -1.73956855e+00, -1.74913935e+00,
	-1.75835773e+00, -1.76725258e+00, -1.77584919e+00,
	-1.78416990e+00, -1.79223453e+00, -1.80006075e+00,
	-1.80766437e+00, -1.81505964e+00, -1.82225940e+00,
	-1.82927530e+00, -1.83611794e+00, -1.84279698e+00,
	-1.84932127e+00, -1.85569892e+00, -1.86193740e+00,
	-1.86804360e+00, -1.87402388e+00, -1.87988413e+00,
	-1.88562983e+00, -1.89126607e+00, -1.89679760e+00,
	-1.90222885e+00, -1.90756395e+00, -1.91280679e+00,
	-1.91796101e+00, -1.92303002e+00, -1.92801704e+00,
	-1.93292509e+00, -1.93775705e+00, -1.94251559e+00,
	-1.94720328e+00, -1.95182252e+00, -1.95637561e+00,
	-1.96086471e+00, -1.96529188e+00, -1.96965908e+00,
	-1.97396817e+00, -1.97822093e+00, -1.98241904e+00,
	-1.98656411e+00, -1.99065768e+00, -1.99470122e+00,
	-1.99869613e+00},
{1.30592376e-04, 3.54780202e-01, 4.22050344e-01,
	4.67149412e-01, 5.02032084e-01, 5.30867858e-01,
	5.55650170e-01, 5.77501296e-01, 5.97121154e-01,
	6.14978238e-01, 6.31402872e-01, 6.46637440e-01,
	6.60865515e-01, 6.74229755e-01, 6.86843408e-01,
	6.98798009e-01, 7.10168688e-01, 7.21017938e-01,
	7.31398341e-01, 7.41354603e-01, 7.50925074e-01,
	7.60142923e-01, 7.69037045e-01, 7.77632782e-01,
	7.85952492e-01, 7.94016007e-01, 8.01841009e-01,
	8.09443333e-01, 8.16837226e-01, 8.24035549e-01,
	8.31049962e-01, 8.37891065e-01, 8.44568531e-01,
	8.51091211e-01, 8.57467223e-01, 8.63704040e-01,
	8.69808551e-01, 8.75787123e-01, 8.81645657e-01,
	8.87389629e-01, 8.93024133e-01, 8.98553916e-01,
	9.03983409e-01, 9.09316756e-01, 9.14557836e-01,
	9.19710291e-01, 9.24777540e-01, 9.29762800e-01,
	9.34669099e-01, 9.39499296e-01, 9.44256090e-01,
	9.48942030e-01, 9.53559531e-01, 9.58110882e-01,
	9.62598250e-01, 9.67023698e-01, 9.71389181e-01,
	9.75696562e-01, 9.79947614e-01, 9.84144025e-01,
	9.88287408e-01, 9.92379299e-01, 9.96421168e-01,
	1.00041442e+00},
{-2.96209812e-06, -2.45794824e-04, -8.18027564e-04,
	-1.19157447e-03, -1.46371229e-03, -1.67529045e-03,
	-1.84698016e-03, -1.99058664e-03, -2.11344205e-03,
	-2.22039065e-03, -2.31478873e-03, -2.39905115e-03,
	-2.47496962e-03, -2.54390793e-03, -2.60692676e-03,
	-2.66486645e-03, -2.71840346e-03, -2.76809003e-03,
	-2.81438252e-03, -2.85766225e-03, -2.89825096e-03,
	-2.93642247e-03, -2.97241172e-03, -3.00642174e-03,
	-3.03862912e-03, -3.06918837e-03, -3.09823546e-03,
	-3.12589065e-03, -3.15226077e-03, -3.17744116e-03,
	-3.20151726e-03, -3.22456591e-03, -3.24665644e-03,
	-3.26785166e-03, -3.28820859e-03, -3.30777919e-03,
	-3.32661092e-03, -3.34474723e-03, -3.36222800e-03,
	-3.37908995e-03, -3.39536690e-03, -3.41109012e-03,
	-3.42628855e-03, -3.44098902e-03, -3.45521647e-03,
	-3.46899410e-03, -3.48234354e-03, -3.49528498e-03,
	-3.50783728e-03, -3.52001812e-03, -3.53184405e-03,
	-3.54333061e-03, -3.55449241e-03, -3.56534320e-03,
	-3.57589590e-03, -3.58616273e-03, -3.59615520e-03,
	-3.60588419e-03, -3.61536000e-03, -3.62459235e-03,
	-3.63359049e-03, -3.64236316e-03, -3.65091867e-03,
	-3.65926491e-03},
{-7.75894750e-06, 3.11294169e-03, 3.41779455e-03,
	3.52160375e-03, 3.55957019e-03, 3.56903631e-03,
	3.56431495e-03, 3.55194570e-03, 3.53526954e-03,
	3.51613008e-03, 3.49560287e-03, 3.47434152e-03,
	3.45275527e-03, 3.43110577e-03, 3.40956242e-03,
	3.38823540e-03, 3.36719598e-03, 3.34648945e-03,
	3.32614343e-03, 3.30617351e-03, 3.28658692e-03,
	3.26738515e-03, 3.24856568e-03, 3.23012330e-03,
	3.21205091e-03, 3.19434023e-03, 3.17698219e-03,
	3.15996727e-03, 3.14328577e-03, 3.12692791e-03,
	3.11088400e-03, 3.09514449e-03, 3.07970007e-03,
	3.06454165e-03, 3.04966043e-03, 3.03504790e-03,
	3.02069585e-03, 3.00659636e-03, 2.99274180e-03,
	2.97912486e-03, 2.96573849e-03, 2.95257590e-03,
	2.93963061e-03, 2.92689635e-03, 2.91436713e-03,
	2.90203718e-03, 2.88990095e-03, 2.87795312e-03,
	2.86618855e-03, 2.85460234e-03, 2.84318974e-03,
	2.83194618e-03, 2.82086729e-03, 2.80994883e-03,
	2.79918673e-03, 2.78857707e-03, 2.77811607e-03,
	2.76780009e-03, 2.75762559e-03, 2.74758919e-03,
	2.73768761e-03, 2.72791768e-03, 2.71827634e-03,
	2.70876064e-03},
{-9.99869423e-01, -6.38561407e-01, -5.69514530e-01,
	-5.23990915e-01, -4.89176780e-01, -4.60615628e-01,
	-4.36195579e-01, -4.14739573e-01, -3.95520699e-01,
	-3.78056805e-01, -3.62010728e-01, -3.47136887e-01,
	-3.33250504e-01, -3.20208824e-01, -3.07899106e-01,
	-2.96230641e-01, -2.85129278e-01, -2.74533563e-01,
	-2.64391946e-01, -2.54660728e-01, -2.45302512e-01,
	-2.36285026e-01, -2.27580207e-01, -2.19163487e-01,
	-2.11013226e-01, -2.03110249e-01, -1.95437482e-01,
	-1.87979648e-01, -1.80723016e-01, -1.73655197e-01,
	-1.66764971e-01, -1.60042136e-01, -1.53477393e-01,
	-1.47062234e-01, -1.40788856e-01, -1.34650080e-01,
	-1.28639289e-01, -1.22750366e-01, -1.16977645e-01,
	-1.11315866e-01, -1.05760138e-01, -1.00305900e-01,
	-9.49488960e-02, -8.96851464e-02, -8.45109223e-02,
	-7.94227260e-02, -7.44172709e-02, -6.94914651e-02,
	-6.46423954e-02, -5.98673139e-02, -5.51636250e-02,
	-5.05288741e-02, -4.59607376e-02, -4.14570134e-02,
	-3.70156122e-02, -3.26345497e-02, -2.83119399e-02,
	-2.40459880e-02, -1.98349851e-02, -1.56773019e-02,
	-1.15713843e-02, -7.51574873e-03, -3.50897732e-03,
	4.50285508e-04},
{1.13389002e-04, 3.50509549e-01, 4.19971782e-01,
	4.66835760e-01, 5.03053790e-01, 5.32907131e-01,
	5.58475931e-01, 5.80942937e-01, 6.01050219e-01,
	6.19296203e-01, 6.36032925e-01, 6.51518847e-01,
	6.65949666e-01, 6.79477330e-01, 6.92222311e-01,
	7.04281836e-01, 7.15735567e-01, 7.26649641e-01,
	7.37079603e-01, 7.47072578e-01, 7.56668915e-01,
	7.65903438e-01, 7.74806427e-01, 7.83404383e-01,
	7.91720644e-01, 7.99775871e-01, 8.07588450e-01,
	8.15174821e-01, 8.22549745e-01, 8.29726527e-01,
	8.36717208e-01, 8.43532720e-01, 8.50183021e-01,
	8.56677208e-01, 8.63023619e-01, 8.69229911e-01,
	8.75303138e-01, 8.81249811e-01, 8.87075954e-01,
	8.92787154e-01, 8.98388600e-01, 9.03885123e-01,
	9.09281227e-01, 9.14581119e-01, 9.19788738e-01,
	9.24907772e-01, 9.29941684e-01, 9.34893728e-01,
	9.39766966e-01, 9.44564285e-01, 9.49288407e-01,
	9.53941905e-01, 9.58527211e-01, 9.63046630e-01,
	9.67502344e-01, 9.71896424e-01, 9.76230838e-01,
	9.80507456e-01, 9.84728057e-01, 9.88894335e-01,
	9.93007906e-01, 9.97070310e-01, 1.00108302e+00,
	1.00504744e+00}};
}
break; */
case FILTER_OFF:
    {
    	// Filter off has gain...
    	float *sp = this->sampleBlock;
    	for (int k=0 ; k < BLOCK_SIZE ; k++) {
			*sp++ = (*sp) * mixerGain;
			*sp++ = (*sp) * mixerGain;
		}
    }
    break;
    default:
    	// NO EFFECT
   	break;
    }

}


void Timbre::afterNewParamsLoad() {
    for (int k = 0; k < params.engine1.numberOfVoice; k++) {
        voices[voiceNumber[k]]->afterNewParamsLoad();
    }

    for (int j=0; j<NUMBER_OF_ENCODERS * 2; j++) {
        this->env1.reloadADSR(j);
        this->env2.reloadADSR(j);
        this->env3.reloadADSR(j);
        this->env4.reloadADSR(j);
        this->env5.reloadADSR(j);
        this->env6.reloadADSR(j);
    }


    resetArpeggiator();

    for (int k=0; k<NUMBER_OF_ENCODERS; k++) {
        setNewEffecParam(k);
    }
    // Update midi note scale
    updateMidiNoteScale(0);
    updateMidiNoteScale(1);
}


void Timbre::resetArpeggiator() {
	// Reset Arpeggiator
	FlushQueue();
	note_stack.Clear();
	setArpeggiatorClock(params.engineArp1.clock);
	setLatchMode(params.engineArp2.latche);
}



void Timbre::setNewValue(int index, struct ParameterDisplay* param, float newValue) {
    if (newValue > param->maxValue) {
        // in v2, matrix target were removed so some values are > to max value but we need to accept it
        bool mustConstraint = true;
		if (param->valueNameOrder != NULL) {
			for (int v = 0; v < param->numberOfValues; v++) {
				if ((int)param->valueNameOrder[v] == (int)(newValue + .01)) {
					mustConstraint = false;
				}
			}
		}
        if (mustConstraint) {
            newValue= param->maxValue;
        }
    } else if (newValue < param->minValue) {
        newValue= param->minValue;
    }
    ((float*)&params)[index] = newValue;
}

int Timbre::getSeqStepValue(int whichStepSeq, int step) {

    if (whichStepSeq == 0) {
        return params.lfoSteps1.steps[step];
    } else {
        return params.lfoSteps2.steps[step];
    }
}

void Timbre::setSeqStepValue(int whichStepSeq, int step, int value) {

    if (whichStepSeq == 0) {
        params.lfoSteps1.steps[step] = value;
    } else {
        params.lfoSteps2.steps[step] = value;
    }
}

void Timbre::recomputeBPValues(float q, float fSquare ) {
    //        /* filter coefficients */
    //        omega1  = 2 * PI * f/srate; // f is your center frequency
    //        sn1 = (float)sin(omega1);
    //        cs1 = (float)cos(omega1);
    //        alpha1 = sn1/(2*Qvalue);        // Qvalue is none other than Q!
    //        a0 = 1.0f + alpha1;     // a0
    //        b0 = alpha1;            // b0
    //        b1 = 0.0f;          // b1/b0
    //        b2= -alpha1/b0          // b2/b0
    //        a1= -2.0f * cs1/a0;     // a1/a0
    //        a2= (1.0f - alpha1)/a0;          // a2/a0
    //        k = b0/a0;

    // frequency must be up to SR / 2.... So 1024 * param1 :
    // 1000 instead of 1024 to get rid of strange border effect....

	//limit low values to avoid cracklings :
	if (fSquare < 0.1f && q < 0.15f) {
		q = 0.15f;
	}

	float sn1 = sinTable[(int)(12 + 1000 * fSquare)];
    // sin(x) = cos( PI/2 - x)
    int cosPhase = 500 - 1000 * fSquare;
    if (cosPhase < 0) {
        cosPhase += 2048;
    }
    float cs1 = sinTable[cosPhase];

    float alpha1 = sn1 * 12.5f;
    if (q > 0) {
        alpha1 = sn1 / ( 8 * q);
    }

    float A0 = 1.0f + alpha1;
    float A0Inv = 1 / A0;

    float B0 = alpha1;
    //fxParamB1 = 0.0f;
    fxParamB2 = - alpha1 * A0Inv;
    fxParamA1 = -2.0f * cs1 * A0Inv;
    fxParamA2 = (1.0f - alpha1) * A0Inv;

    fxParam1 = B0 * A0Inv;
}

void Timbre::setNewEffecParam(int encoder) {
	if (encoder == 0) {
   		v0L = v1L = v2L = v3L = v4L = v5L = v6L = v7L = v8L = v0R = v1R = v2R = v3R = v4R = v5R = v6R = v7R = v8R = v8R = 0.0f;

	    for (int k=1; k<NUMBER_OF_ENCODERS; k++) {
	        setNewEffecParam(k);
	    }
	}
	switch ((int)params.effect.type) {
    	case FILTER_BASS:
    		// Selectivity = fxParam1
    		// ratio = fxParam2
    		// gain1 = fxParam3
    		fxParam1 = 50 + 200 * params.effect.param1;
    		fxParam2 = params.effect.param2 * 4;
    		fxParam3 = 1.0f/(fxParam1 + 1.0f);
    		break;
    	case FILTER_HP:
    	case FILTER_LP:
        case FILTER_TILT:
    		switch (encoder) {
    		case ENCODER_EFFECT_TYPE:
    			fxParam2 = 0.3f - params.effect.param2 * 0.3f;
    			break;
    		case ENCODER_EFFECT_PARAM1:
    			// Done in every loop
    			// fxParam1 = pow(0.5, (128- (params.effect.param1 * 128))   / 16.0);
    			break;
    		case ENCODER_EFFECT_PARAM2:
    	    	// fxParam2 = pow(0.5, ((params.effect.param2 * 127)+24) / 16.0);
    			// => value from 0.35 to 0.0
    			fxParam2 = 0.27f - params.effect.param2 * 0.27f;
    			break;
    		}
        	break;
        case FILTER_LP2:
        case FILTER_HP2:
		case FILTER_LPHP:
		    switch (encoder) {
    		case ENCODER_EFFECT_TYPE:
    			fxParam2 = 0.27f - params.effect.param2 * 0.267f;
    			break;
    		case ENCODER_EFFECT_PARAM2:
    			fxParam2 = 0.27f - params.effect.param2 * 0.267f;
    			break;
    		}
        	break;
        case FILTER_CRUSHER:
        {
            if (encoder == ENCODER_EFFECT_PARAM2) {
                fxParam1 = pow(2, (int)(1.0f + 15.0f * params.effect.param2));
                fxParam2 = 1 / fxParam1;
            }
            break;
        }
        case FILTER_BP:
        case FILTER_BP2:
        {
            fxParam1PlusMatrix = -1.0f;
            break;
        }
		case FILTER_SIGMOID:
		case FILTER_FOLD:
		case FILTER_WRAP:
		  	switch (encoder) {
    		case ENCODER_EFFECT_PARAM2:
				fxParam2 = 0.1f + (params.effect.param2 * params.effect.param2);
				break;
    		}
			break;
		case FILTER_TEXTURE1:
		case FILTER_TEXTURE2:
		  	switch (encoder) {
    		case ENCODER_EFFECT_PARAM2:
				fxParam2 = sqrt3(1 - params.effect.param2 * 0.99f);
				break;
    		}
			break;
		default:
		  	switch (encoder) {
    		case ENCODER_EFFECT_TYPE:
    			break;
    		case ENCODER_EFFECT_PARAM1:
    			break;
    		case ENCODER_EFFECT_PARAM2:
    			fxParam2 = params.effect.param2;
    			break;
    		}
	}
}

// Code bellowed have been adapted by Xavier Hosxe for PreenFM2
// It come from Muteable Instrument midiPAL

/////////////////////////////////////////////////////////////////////////////////
// Copyright 2011 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// -----------------------------------------------------------------------------
//
// Arpeggiator app.



void Timbre::arpeggiatorNoteOn(uint8_t note, uint8_t velocity) {
	// CLOCK_MODE_INTERNAL
	if (params.engineArp1.clock == CLOCK_INTERNAL) {
		if (idle_ticks_ >= 96 || !running_) {
			Start();
		}
		idle_ticks_ = 0;
	}

	if (latch_ && !recording_) {
		note_stack.Clear();
		recording_ = 1;
	}
	note_stack.NoteOn(note, velocity);
}


void Timbre::arpeggiatorNoteOff(uint8_t note) {
	if (ignore_note_off_messages_) {
		return;
	}
	if (!latch_) {
		note_stack.NoteOff(note);
	} else {
		if (note == note_stack.most_recent_note().note) {
			recording_ = 0;
		}
	}
}


void Timbre::OnMidiContinue() {
	if (params.engineArp1.clock == CLOCK_EXTERNAL) {
		running_ = 1;
	}
}

void Timbre::OnMidiStart() {
	if (params.engineArp1.clock == CLOCK_EXTERNAL) {
		Start();
	}
}

void Timbre::OnMidiStop() {
	if (params.engineArp1.clock == CLOCK_EXTERNAL) {
		running_ = 0;
		FlushQueue();
	}
}


void Timbre::OnMidiClock() {
	if (params.engineArp1.clock == CLOCK_EXTERNAL && running_) {
		Tick();
	}
}


void Timbre::SendNote(uint8_t note, uint8_t velocity) {

	// If there are some Note Off messages for the note about to be triggeered
	// remove them from the queue and process them now.
	if (event_scheduler.Remove(note, 0)) {
		preenNoteOff(note);
	}

	// Send a note on and schedule a note off later.
	preenNoteOn(note, velocity);
	event_scheduler.Schedule(note, 0, midi_clock_tick_per_step[(int)params.engineArp2.duration] - 1, 0);
}

void Timbre::SendLater(uint8_t note, uint8_t velocity, uint8_t when, uint8_t tag) {
	event_scheduler.Schedule(note, velocity, when, tag);
}


void Timbre::SendScheduledNotes() {
  uint8_t current = event_scheduler.root();
  while (current) {
    const SchedulerEntry& entry = event_scheduler.entry(current);
    if (entry.when) {
      break;
    }
    if (entry.note != kZombieSlot) {
      if (entry.velocity == 0) {
    	  preenNoteOff(entry.note);
      } else {
    	  preenNoteOn(entry.note, entry.velocity);
      }
    }
    current = entry.next;
  }
  event_scheduler.Tick();
}


void Timbre::FlushQueue() {
  while (event_scheduler.size()) {
    SendScheduledNotes();
  }
}



void Timbre::Tick() {
	++tick_;

	if (note_stack.size()) {
		idle_ticks_ = 0;
	}
	++idle_ticks_;
	if (idle_ticks_ >= 96) {
		idle_ticks_ = 96;
	    if (params.engineArp1.clock == CLOCK_INTERNAL) {
	      running_ = 0;
	      FlushQueue();
	    }
	}

	SendScheduledNotes();

	if (tick_ >= midi_clock_tick_per_step[(int)params.engineArp2.division]) {
		tick_ = 0;
		uint16_t pattern = getArpeggiatorPattern();
		uint8_t has_arpeggiator_note = (bitmask_ & pattern) ? 255 : 0;
		const int num_notes = note_stack.size();
		const int direction = params.engineArp1.direction;

		if (num_notes && has_arpeggiator_note) {
			if ( ARPEGGIO_DIRECTION_CHORD != direction ) {
				StepArpeggio();
				int step, transpose = 0;
				if ( current_direction_ > 0 ) {
					step = start_step_ + current_step_;
					if ( step >= num_notes ) {
						step -= num_notes;
						transpose = 12;
					}
				} else {
					step = (num_notes - 1) - (start_step_ + current_step_);
					if ( step < 0 ) {
						step += num_notes;
						transpose = -12;
					}
				}
#ifdef DEBUG_ARP_STEP
				lcd.setRealTimeAction(true);
				lcd.setCursor(16,0);
				lcd.print( current_direction_ > 0 ? '+' : '-' );
				lcd.print( step );
				lcd.setRealTimeAction(false);
#endif
				const NoteEntry &noteEntry = ARPEGGIO_DIRECTION_PLAYED == direction
					? note_stack.played_note(step)
					: note_stack.sorted_note(step);

				uint8_t note = noteEntry.note;
				uint8_t velocity = noteEntry.velocity;
				note += 12 * current_octave_;
				if ( __canTranspose( direction ) )
					 note += transpose;

				while (note > 127) {
					note -= 12;
				}

				SendNote(note, velocity);
			} else {
				for (int i = 0; i < note_stack.size(); ++i ) {
					const NoteEntry& noteEntry = note_stack.sorted_note(i);
					SendNote(noteEntry.note, noteEntry.velocity);
				}
			}
		}
		bitmask_ <<= 1;
		if (!bitmask_) {
			bitmask_ = 1;
		}
	}
}



void Timbre::StepArpeggio() {

	if (current_octave_ == 127) {
		StartArpeggio();
		return;
	}

	int direction = params.engineArp1.direction;
	uint8_t num_notes = note_stack.size();
	if (direction == ARPEGGIO_DIRECTION_RANDOM) {
		uint8_t random_byte = *(uint8_t*)noise;
		current_octave_ = random_byte & 0xf;
		current_step_ = (random_byte & 0xf0) >> 4;
		while (current_octave_ >= params.engineArp1.octave) {
			current_octave_ -= params.engineArp1.octave;
		}
		while (current_step_ >= num_notes) {
			current_step_ -= num_notes;
		}
	} else {
		// NOTE: We always count [0 - num_notes) here; the actual handling of direction is in Tick()

		uint8_t trigger_change = 0;
		if (++current_step_ >= num_notes) {
			current_step_ = 0;
			trigger_change = 1;
		}

		// special case the 'ROTATE' and 'SHIFT' modes, they might not change the octave until the cycle is through
		if (trigger_change && (direction >= ARPEGGIO_DIRECTION_ROTATE_UP ) ) {
			if ( ++start_step_ >= num_notes )
				start_step_ = 0;
			else
				trigger_change = 0;
		}

		if (trigger_change) {
			current_octave_ += current_direction_;
			if (current_octave_ >= params.engineArp1.octave || current_octave_ < 0) {
				if ( __canChangeDir(direction) ) {
					current_direction_ = -current_direction_;
					StartArpeggio();
					if (num_notes > 1 || params.engineArp1.octave > 1) {
						StepArpeggio();
					}
				} else {
					StartArpeggio();
				}
			}
		}
	}
}

void Timbre::StartArpeggio() {

	current_step_ = 0;
	start_step_ = 0;
	if (current_direction_ == 1) {
		current_octave_ = 0;
	} else {
		current_octave_ = params.engineArp1.octave - 1;
	}
}

void Timbre::Start() {
	bitmask_ = 1;
	recording_ = 0;
	running_ = 1;
	tick_ = midi_clock_tick_per_step[(int)params.engineArp2.division] - 1;
    current_octave_ = 127;
	current_direction_ = __getDirection( params.engineArp1.direction );
}


void Timbre::arpeggiatorSetHoldPedal(uint8_t value) {
  if (ignore_note_off_messages_ && !value) {
    // Pedal was released, kill all pending arpeggios.
    note_stack.Clear();
  }
  ignore_note_off_messages_ = value;
}


void Timbre::setLatchMode(uint8_t value) {
    // When disabling latch mode, clear the note stack.
	latch_ = value;
    if (value == 0) {
      note_stack.Clear();
      recording_ = 0;
    }
}

void Timbre::setDirection(uint8_t value) {
	// When changing the arpeggio direction, reset the pattern.
	current_direction_ = __getDirection(value);
	StartArpeggio();
}

void Timbre::lfoValueChange(int currentRow, int encoder, float newValue) {
    for (int k = 0; k < params.engine1.numberOfVoice; k++) {
        voices[voiceNumber[k]]->lfoValueChange(currentRow, encoder, newValue);
    }
}

void Timbre::updateMidiNoteScale(int scale) {

    int intBreakNote;
    int curveBefore;
    int curveAfter;
    if (scale == 0) {
        intBreakNote = params.midiNote1Curve.breakNote;
        curveBefore = params.midiNote1Curve.curveBefore;
        curveAfter = params.midiNote1Curve.curveAfter;
    } else {
        intBreakNote = params.midiNote2Curve.breakNote;
        curveBefore = params.midiNote2Curve.curveBefore;
        curveAfter = params.midiNote2Curve.curveAfter;
    }
    float floatBreakNote = intBreakNote;
    float multiplier = 1.0f;


    switch (curveBefore) {
    case MIDI_NOTE_CURVE_FLAT:
        for (int n=0; n < intBreakNote ; n++) {
            midiNoteScale[scale][timbreNumber][n] = 0;
        }
        break;
    case MIDI_NOTE_CURVE_M_LINEAR:
        multiplier = -1.0f;
        goto linearBefore;
    case MIDI_NOTE_CURVE_M_LINEAR2:
        multiplier = -8.0f;
        goto linearBefore;
    case MIDI_NOTE_CURVE_LINEAR2:
        multiplier = 8.0f;
        goto linearBefore;
    case MIDI_NOTE_CURVE_LINEAR:
        linearBefore:
        for (int n=0; n < intBreakNote ; n++) {
            float fn = (floatBreakNote - n);
            midiNoteScale[scale][timbreNumber][n] = fn * INV127 * multiplier;
        }
        break;
    case MIDI_NOTE_CURVE_M_EXP:
        multiplier = -1.0f;
    case MIDI_NOTE_CURVE_EXP:
        for (int n=0; n < intBreakNote ; n++) {
            float fn = (floatBreakNote - n);
            fn = fn * fn / floatBreakNote;
            midiNoteScale[scale][timbreNumber][n] = fn * INV16 * multiplier;
        }
        break;
    }

    // BREAK NOTE = 0;
    midiNoteScale[scale][timbreNumber][intBreakNote] = 0;


    float floatAfterBreakNote = 127 - floatBreakNote;
    int intAfterBreakNote = 127 - intBreakNote;


    switch (curveAfter) {
    case MIDI_NOTE_CURVE_FLAT:
        for (int n = intBreakNote + 1; n < 128 ; n++) {
            midiNoteScale[scale][timbreNumber][n] = 0;
        }
        break;
    case MIDI_NOTE_CURVE_M_LINEAR:
        multiplier = -1.0f;
        goto linearAfter;
    case MIDI_NOTE_CURVE_M_LINEAR2:
        multiplier = -8.0f;
        goto linearAfter;
    case MIDI_NOTE_CURVE_LINEAR2:
        multiplier = 8.0f;
        goto linearAfter;
    case MIDI_NOTE_CURVE_LINEAR:
        linearAfter:
        for (int n = intBreakNote + 1; n < 128 ; n++) {
            float fn = n - floatBreakNote;
            midiNoteScale[scale][timbreNumber][n] = fn  * INV127 * multiplier;
        }
        break;
    case MIDI_NOTE_CURVE_M_EXP:
        multiplier = -1.0f;
    case MIDI_NOTE_CURVE_EXP:
        for (int n = intBreakNote + 1; n < 128 ; n++) {
            float fn = n - floatBreakNote;
            fn = fn * fn / floatBreakNote;
            midiNoteScale[scale][timbreNumber][n] = fn * INV16 * multiplier;
        }
        break;
    }
/*
    lcd.setCursor(0,0);
    lcd.print((int)(midiNoteScale[timbreNumber][25] * 127.0f));
    lcd.print(" ");
    lcd.setCursor(10,0);
    lcd.print((int)(midiNoteScale[timbreNumber][intBreakNote - 5] * 127.0f));
    lcd.print(" ");
    lcd.setCursor(0,1);
    lcd.print((int)(midiNoteScale[timbreNumber][intBreakNote + 5] * 127.0f));
    lcd.print(" ");
    lcd.setCursor(10,1);
    lcd.print((int)(midiNoteScale[timbreNumber][102] * 127.0f));
    lcd.print(" ");
*/

}




void Timbre::midiClockContinue(int songPosition) {

    for (int k = 0; k < params.engine1.numberOfVoice; k++) {
        voices[voiceNumber[k]]->midiClockContinue(songPosition);
    }

    this->recomputeNext = ((songPosition&0x1)==0);
    OnMidiContinue();
}


void Timbre::midiClockStart() {

    for (int k = 0; k < params.engine1.numberOfVoice; k++) {
        voices[voiceNumber[k]]->midiClockStart();
    }

    this->recomputeNext = true;
    OnMidiStart();
}

void Timbre::midiClockSongPositionStep(int songPosition) {

    for (int k = 0; k < params.engine1.numberOfVoice; k++) {
        voices[voiceNumber[k]]->midiClockSongPositionStep(songPosition,  this->recomputeNext);
    }

    if ((songPosition & 0x1)==0) {
        this->recomputeNext = true;
    }
}


void Timbre::resetMatrixDestination(float oldValue) {
    for (int k = 0; k < params.engine1.numberOfVoice; k++) {
        voices[voiceNumber[k]]->matrix.resetDestination(oldValue);
    }
}

void Timbre::setMatrixSource(enum SourceEnum source, float newValue) {
    for (int k = 0; k < params.engine1.numberOfVoice; k++) {
        voices[voiceNumber[k]]->matrix.setSource(source, newValue);
    }
}


void Timbre::verifyLfoUsed(int encoder, float oldValue, float newValue) {
    // No need to recompute
    if (params.engine1.numberOfVoice == 0.0f) {
        return;
    }
    if (encoder == ENCODER_MATRIX_MUL && oldValue != 0.0f && newValue != 0.0f) {
        return;
    }

    for (int lfo=0; lfo < NUMBER_OF_LFO; lfo++) {
        lfoUSed[lfo] = 0;
    }

    MatrixRowParams* matrixRows = &params.matrixRowState1;


    for (int r = 0; r < MATRIX_SIZE; r++) {
        if (matrixRows[r].source >= MATRIX_SOURCE_LFO1 && matrixRows[r].source <= MATRIX_SOURCE_LFOSEQ2
                && matrixRows[r].mul != 0.0f
                && matrixRows[r].destination != 0.0f) {
            lfoUSed[(int)matrixRows[r].source - MATRIX_SOURCE_LFO1]++;
        }


		// Check if we have a Mtx* that would require LFO even if mul is 0
		// http://ixox.fr/forum/index.php?topic=69220.0
        if (matrixRows[r].destination >= MTX1_MUL && matrixRows[r].destination <= MTX4_MUL && matrixRows[r].mul != 0.0f && matrixRows[r].source != 0.0f) {
			int index = matrixRows[r].destination - MTX1_MUL;
	        if (matrixRows[index].source >= MATRIX_SOURCE_LFO1 && matrixRows[index].source <= MATRIX_SOURCE_LFOSEQ2 && matrixRows[index].destination != 0.0f) {
            	lfoUSed[(int)matrixRows[index].source - MATRIX_SOURCE_LFO1]++;
			}
		}

    }

    /*
    lcd.setCursor(11, 1);
    lcd.print('>');
    for (int lfo=0; lfo < NUMBER_OF_LFO; lfo++) {
        lcd.print((int)lfoUSed[lfo]);
    }
    lcd.print('<');
	*/

}
