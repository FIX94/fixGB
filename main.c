/*
 * Copyright (C) 2017 FIX94
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <malloc.h>
#include <inttypes.h>
#include <GL/glut.h>
#include <GL/glext.h>
#include <time.h>
#include <math.h>
#include "cpu.h"
#include "input.h"
#include "ppu.h"
#include "mem.h"
#include "apu.h"
#include "audio.h"

#define DEBUG_HZ 0
#define DEBUG_MAIN_CALLS 0
#define DEBUG_KEY 0
#define DEBUG_LOAD_INFO 1

static const char *VERSION_STRING = "fixGB Alpha v0.5.1";

static void gbEmuDisplayFrame(void);
static void gbEmuMainLoop(void);
static void gbEmuDeinit(void);

static void gbEmuHandleKeyDown(unsigned char key, int x, int y);
static void gbEmuHandleKeyUp(unsigned char key, int x, int y);
static void gbEmuHandleSpecialDown(int key, int x, int y);
static void gbEmuHandleSpecialUp(int key, int x, int y);

uint8_t *emuGBROM = NULL;
char *emuSaveName = NULL;
//used externally
uint32_t textureImage[0x9A00];
bool nesPause = false;
bool ppuDebugPauseFrame = false;
bool gbEmuGBSPlayback = false;
bool gbsTimerMode = false;
uint16_t gbsLoadAddr = 0;
uint16_t gbsInitAddr = 0;
uint16_t gbsPlayAddr = 0;
uint16_t gbsSP = 0;
uint8_t gbsTracksTotal = 0, gbsTMA = 0, gbsTAC = 0;
uint8_t cpuTimer = 3;
bool allowCgbRegs = false;

static bool inPause = false;
static bool inResize = false;

#if WINDOWS_BUILD
#include <windows.h>
typedef bool (APIENTRY *PFNWGLSWAPINTERVALEXTPROC) (int interval);
PFNWGLSWAPINTERVALEXTPROC wglSwapIntervalEXT = NULL;
#if DEBUG_HZ
static DWORD emuFrameStart = 0;
static DWORD emuTimesCalled = 0;
static DWORD emuTotalElapsed = 0;
#endif
#if DEBUG_MAIN_CALLS
static DWORD emuMainFrameStart = 0;
static DWORD emuMainTimesCalled = 0;
static DWORD emuMainTimesSkipped = 0;
static DWORD emuMainTotalElapsed = 0;
#endif
#endif

#define VISIBLE_DOTS 160
#define VISIBLE_LINES 144

static const uint32_t visibleImg = VISIBLE_DOTS*VISIBLE_LINES*4;
static uint8_t scaleFactor = 3;
static uint32_t mainLoopRuns;
static uint16_t mainLoopPos;
//from input.c
extern uint8_t inValReads[8];

int main(int argc, char** argv)
{
	puts(VERSION_STRING);
	if(argc >= 2 && (strstr(argv[1],".gbs") != NULL || strstr(argv[1],".GBS") != NULL))
	{
		FILE *gbF = fopen(argv[1],"rb");
		if(!gbF) return EXIT_SUCCESS;
		fseek(gbF,0,SEEK_END);
		size_t fsize = ftell(gbF);
		rewind(gbF);
		uint8_t *tmpROM = malloc(fsize);
		fread(tmpROM,1,fsize,gbF);
		fclose(gbF);
		gbsTracksTotal = tmpROM[4];
		gbsLoadAddr = (tmpROM[6])|(tmpROM[7]<<8);
		gbsInitAddr = (tmpROM[8])|(tmpROM[9]<<8);
		gbsPlayAddr = (tmpROM[0xA])|(tmpROM[0xB]<<8);
		gbsSP = (tmpROM[0xC])|(tmpROM[0xD]<<8);
		//should give more than enough room for everything
		uint32_t totalROMsize = (fsize-0x70+gbsLoadAddr+0x7FFF)&(~0x7FFF);
		emuGBROM = malloc(totalROMsize);
		memset(emuGBROM,0xFF,totalROMsize);
		memcpy(emuGBROM+gbsLoadAddr,tmpROM+0x70,fsize-0x70);
		memInit(true,true);
		gbsTMA = tmpROM[0xE];
		gbsTAC = tmpROM[0xF];
		if(gbsTAC&4)
		{
			printf("Play Timing: Timer\n");
			gbsTimerMode = true;
		}
		else
		{
			printf("Play Timing: VSync\n");
			gbsTimerMode = false;
		}
		if(gbsTAC&0x80)
		{
			cpuSetSpeed(true);
			allowCgbRegs = true;
		}
		else
		{
			cpuSetSpeed(false);
			allowCgbRegs = false;
		}
		if(tmpROM[0x10] != 0)
			printf("Game: %.32s\n",(char*)(tmpROM+0x10));
		free(tmpROM);
		apuInitBufs();
		//does all inits for us
		memStartGBS();
		gbEmuGBSPlayback = true;
	}
	else if(argc >= 2 && (strstr(argv[1],".gbc") != NULL || strstr(argv[1],".GBC") != NULL
						|| strstr(argv[1],".gb") != NULL || strstr(argv[1],".GB") != NULL))
	{
		FILE *gbF = fopen(argv[1],"rb");
		if(!gbF) return EXIT_SUCCESS;
		fseek(gbF,0,SEEK_END);
		size_t fsize = ftell(gbF);
		rewind(gbF);
		emuGBROM = malloc(fsize);
		if(emuGBROM == NULL)
		{
			printf("Unable to allocate ROM space...\n");
			exit(EXIT_SUCCESS);
		}
		fread(emuGBROM,1,fsize,gbF);
		fclose(gbF);
		if(strstr(argv[1],".gbc") != NULL || strstr(argv[1],".GBC") != NULL)
		{
			emuSaveName = malloc(strlen(argv[1])+1);
			memcpy(emuSaveName,argv[1],strlen(argv[1])+1);
			memcpy(emuSaveName+strlen(argv[1])-3,"sav",3);
		}
		else if(strstr(argv[1],".gb") != NULL || strstr(argv[1],".GB") != NULL)
		{
			emuSaveName = malloc(strlen(argv[1])+2);
			memcpy(emuSaveName,argv[1],strlen(argv[1])+1);
			memcpy(emuSaveName+strlen(argv[1])-2,"sav",4);
		}
		//Set CGB Regs allowed
		allowCgbRegs = !!(emuGBROM[0x143]&0x80);
		printf("CGB Regs are %sallowed\n", allowCgbRegs?"":"dis");
		if(!memInit(true,false))
		{
			free(emuGBROM);
			printf("Exit...\n");
			exit(EXIT_SUCCESS);
		}
		//CPU DMG Mode
		cpuSetSpeed(false);
		apuInitBufs();
		cpuInit();
		ppuInit();
		apuInit();
		inputInit();
		#if DEBUG_LOAD_INFO
		printf("Read in %s\n", argv[1]);
		//printf("Used Mapper: %i\n", mapper);
		//printf("PRG: 0x%x bytes PRG RAM: 0x%x bytes CHR: 0x%x bytes\n", prgROMsize, emuPrgRAMsize, chrROMsize);
		#endif
	}
	if(emuGBROM == NULL)
		return EXIT_SUCCESS;
	#if WINDOWS_BUILD
	#if DEBUG_HZ
	emuFrameStart = GetTickCount();
	#endif
	#if DEBUG_MAIN_CALLS
	emuMainFrameStart = GetTickCount();
	#endif
	#endif
	memset(textureImage,0,visibleImg);
	//do one scanline per idle loop
	mainLoopRuns = 70224;
	mainLoopPos = mainLoopRuns;
	glutInit(&argc, argv);
	glutInitWindowSize(VISIBLE_DOTS*scaleFactor, VISIBLE_LINES*scaleFactor);
	glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGBA);
	glutCreateWindow(VERSION_STRING);
	audioInit();
	atexit(&gbEmuDeinit);
	glutKeyboardFunc(&gbEmuHandleKeyDown);
	glutKeyboardUpFunc(&gbEmuHandleKeyUp);
	glutSpecialFunc(&gbEmuHandleSpecialDown);
	glutSpecialUpFunc(&gbEmuHandleSpecialUp);
	glutDisplayFunc(&gbEmuDisplayFrame);
	glutIdleFunc(&gbEmuMainLoop);
	#if WINDOWS_BUILD
	/* Enable OpenGL VSync */
	wglSwapIntervalEXT = (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
	wglSwapIntervalEXT(1);
	#endif
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, 4, VISIBLE_DOTS, VISIBLE_LINES, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, textureImage);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glEnable(GL_TEXTURE_2D);
	glShadeModel(GL_FLAT);

	glutMainLoop();

	return EXIT_SUCCESS;
}

static volatile bool emuRenderFrame = false;

static void gbEmuDeinit(void)
{
	//printf("\n");
	emuRenderFrame = false;
	audioDeinit();
	apuDeinitBufs();
	if(emuGBROM != NULL)
		free(emuGBROM);
	emuGBROM = NULL;
	memSaveGame();
	if(emuSaveName != NULL)
		free(emuSaveName);
	emuSaveName = NULL;
	//printf("Bye!\n");
}

//used externally
bool emuSkipVsync = false;
bool emuSkipFrame = false;

static uint8_t mainClock = 0;
static uint8_t memClock = 0;

static void gbEmuMainLoop(void)
{
	//do one scanline loop
	do
	{
		if((!emuSkipVsync && emuRenderFrame) || nesPause)
		{
			#if (WINDOWS_BUILD && DEBUG_MAIN_CALLS)
			emuMainTimesSkipped++;
			#endif
			audioSleep();
			return;
		}
		//run APU first to make sure its synced
		if(!(mainClock&15) && !apuCycle())
		{
			#if (WINDOWS_BUILD && DEBUG_MAIN_CALLS)
			emuMainTimesSkipped++;
			#endif
			audioSleep();
			return;
		}
		//channel timer updates
		apuClockTimers();
		//run possible DMA next
		memDmaClockTimers();
		//run CPU (and mem clocks) next
		if(!(mainClock&cpuTimer))
		{
			//main CPU clock
			if(!cpuCycle())
			{
				//memDumpMainMem();
				exit(EXIT_SUCCESS);
			}
			//mem clock tied to CPU clock, so
			//double speed in CGB mode!
			if(!(memClock&3))
				memClockTimers();
			memClock++;
		}
		//run PPU last
		if(!ppuCycle())
			exit(EXIT_SUCCESS);
		if(ppuDrawDone())
		{
			if(!gbEmuGBSPlayback)
			{
				emuRenderFrame = true;
				//update console stats if requested
				#if (WINDOWS_BUILD && DEBUG_HZ)
				emuTimesCalled++;
				DWORD end = GetTickCount();
				emuTotalElapsed += end - emuFrameStart;
				if(emuTotalElapsed >= 1000)
				{
					printf("\r%iHz   ", emuTimesCalled);
					emuTimesCalled = 0;
					emuTotalElapsed = 0;
				}
				emuFrameStart = end;
				#endif
				glutPostRedisplay();
				if(ppuDebugPauseFrame)
					nesPause = true;
			}
			else if(!gbsTimerMode)
				cpuPlayGBS();
		}
		mainClock++;
	}
	while(mainLoopPos--);
	mainLoopPos = mainLoopRuns;
	//update console stats if requested
	#if (WINDOWS_BUILD && DEBUG_MAIN_CALLS)
	emuMainTimesCalled++;
	DWORD end = GetTickCount();
	emuMainTotalElapsed += end - emuMainFrameStart;
	if(emuMainTotalElapsed >= 1000)
	{
		printf("\r%i calls, %i skips   ", emuMainTimesCalled, emuMainTimesSkipped);
		emuMainTimesCalled = 0;
		emuMainTimesSkipped = 0;
		emuMainTotalElapsed = 0;
	}
	emuMainFrameStart = end;
	#endif
}

static void gbEmuHandleKeyDown(unsigned char key, int x, int y)
{
	(void)x;
	(void)y;
	switch (key)
	{
		case 'y':
		case 'z':
		case 'Y':
		case 'Z':
			#if DEBUG_KEY
			if(inValReads[BUTTON_A]==0)
				printf("a\n");
			#endif
			inValReads[BUTTON_A]=1;
			break;
		case 'x':
		case 'X':
			#if DEBUG_KEY
			if(inValReads[BUTTON_B]==0)
				printf("b\n");
			#endif
			inValReads[BUTTON_B]=1;
			break;
		case 's':
		case 'S':
			#if DEBUG_KEY
			if(inValReads[BUTTON_SELECT]==0)
				printf("sel\n");
			#endif
			inValReads[BUTTON_SELECT]=1;
			break;
		case 'a':
		case 'A':
			#if DEBUG_KEY
			if(inValReads[BUTTON_START]==0)
				printf("start\n");
			#endif
			inValReads[BUTTON_START]=1;
			break;
		case '\x1B': //Escape
			//memDumpMainMem();
			exit(EXIT_SUCCESS);
			break;
		case 'p':
		case 'P':
			if(!inPause)
			{
				#if DEBUG_KEY
				printf("pause\n");
				#endif
				inPause = true;
				nesPause ^= true;
			}
			break;
		case '1':
			if(!inResize)
			{
				inResize = true;
				glutReshapeWindow(VISIBLE_DOTS*1, VISIBLE_LINES*1);
			}
			break;
		case '2':
			if(!inResize)
			{
				inResize = true;
				glutReshapeWindow(VISIBLE_DOTS*2, VISIBLE_LINES*2);
			}
			break;
		case '3':
			if(!inResize)
			{
				inResize = true;
				glutReshapeWindow(VISIBLE_DOTS*3, VISIBLE_LINES*3);
			}
			break;
		case '4':
			if(!inResize)
			{
				inResize = true;
				glutReshapeWindow(VISIBLE_DOTS*4, VISIBLE_LINES*4);
			}
			break;
		case '5':
			if(!inResize)
			{
				inResize = true;
				glutReshapeWindow(VISIBLE_DOTS*5, VISIBLE_LINES*5);
			}
			break;
		case '6':
			if(!inResize)
			{
				inResize = true;
				glutReshapeWindow(VISIBLE_DOTS*6, VISIBLE_LINES*6);
			}
			break;
		case '7':
			if(!inResize)
			{
				inResize = true;
				glutReshapeWindow(VISIBLE_DOTS*7, VISIBLE_LINES*7);
			}
			break;
		case '8':
			if(!inResize)
			{
				inResize = true;
				glutReshapeWindow(VISIBLE_DOTS*8, VISIBLE_LINES*8);
			}
			break;
		case '9':
			if(!inResize)
			{
				inResize = true;
				glutReshapeWindow(VISIBLE_DOTS*9, VISIBLE_LINES*9);
			}
			break;
		default:
			break;
	}
}

static void gbEmuHandleKeyUp(unsigned char key, int x, int y)
{
	(void)x;
	(void)y;
	switch (key)
	{
		case 'y':
		case 'z':
		case 'Y':
		case 'Z':
			#if DEBUG_KEY
			printf("a up\n");
			#endif
			inValReads[BUTTON_A]=0;
			break;
		case 'x':
		case 'X':
			#if DEBUG_KEY
			printf("b up\n");
			#endif
			inValReads[BUTTON_B]=0;
			break;
		case 's':
		case 'S':
			#if DEBUG_KEY
			printf("sel up\n");
			#endif
			inValReads[BUTTON_SELECT]=0;
			break;
		case 'a':
		case 'A':
			#if DEBUG_KEY
			printf("start up\n");
			#endif
			inValReads[BUTTON_START]=0;
			break;
		case 'p':
		case 'P':
			#if DEBUG_KEY
			printf("pause up\n");
			#endif
			inPause=false;
			break;
		case '1': case '2':	case '3':
		case '4': case '5':	case '6':
		case '7': case '8':	case '9':
			inResize = false;
			break;
		default:
			break;
	}
}

static void gbEmuHandleSpecialDown(int key, int x, int y)
{
	(void)x;
	(void)y;
	switch(key)
	{
		case GLUT_KEY_UP:
			#if DEBUG_KEY
			if(inValReads[BUTTON_UP]==0)
				printf("up\n");
			#endif
			inValReads[BUTTON_UP]=1;
			break;	
		case GLUT_KEY_DOWN:
			#if DEBUG_KEY
			if(inValReads[BUTTON_DOWN]==0)
				printf("down\n");
			#endif
			inValReads[BUTTON_DOWN]=1;
			break;
		case GLUT_KEY_LEFT:
			#if DEBUG_KEY
			if(inValReads[BUTTON_LEFT]==0)
				printf("left\n");
			#endif
			inValReads[BUTTON_LEFT]=1;
			break;
		case GLUT_KEY_RIGHT:
			#if DEBUG_KEY
			if(inValReads[BUTTON_RIGHT]==0)
				printf("right\n");
			#endif
			inValReads[BUTTON_RIGHT]=1;
			break;
		default:
			break;
	}
}

static void gbEmuHandleSpecialUp(int key, int x, int y)
{
	(void)x;
	(void)y;
	switch(key)
	{
		case GLUT_KEY_UP:
			#if DEBUG_KEY
			printf("up up\n");
			#endif
			inValReads[BUTTON_UP]=0;
			break;	
		case GLUT_KEY_DOWN:
			#if DEBUG_KEY
			printf("down up\n");
			#endif
			inValReads[BUTTON_DOWN]=0;
			break;
		case GLUT_KEY_LEFT:
			#if DEBUG_KEY
			printf("left up\n");
			#endif
			inValReads[BUTTON_LEFT]=0;
			break;
		case GLUT_KEY_RIGHT:
			#if DEBUG_KEY
			printf("right up\n");
			#endif
			inValReads[BUTTON_RIGHT]=0;
			break;
		default:
			break;
	}
}

static void gbEmuDisplayFrame()
{
	if(emuRenderFrame)
	{
		if(emuSkipFrame)
		{
			emuRenderFrame = false;
			return;
		}
		if(textureImage != NULL)
			glTexImage2D(GL_TEXTURE_2D, 0, 4, VISIBLE_DOTS, VISIBLE_LINES, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, textureImage);
		emuRenderFrame = false;
	}

	glClear(GL_COLOR_BUFFER_BIT);

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, glutGet(GLUT_WINDOW_WIDTH), 0, glutGet(GLUT_WINDOW_HEIGHT), -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	double upscaleVal = round((((double)glutGet(GLUT_WINDOW_HEIGHT))/((double)VISIBLE_LINES))*20.0)/20.0;
	double windowMiddle = ((double)glutGet(GLUT_WINDOW_WIDTH))/2.0;
	double drawMiddle = (((double)VISIBLE_DOTS)*upscaleVal)/2.0;
	double drawHeight = ((double)VISIBLE_LINES)*upscaleVal;

	glBegin(GL_QUADS);
		glTexCoord2f(0,0); glVertex2f(windowMiddle-drawMiddle,drawHeight);
		glTexCoord2f(1,0); glVertex2f(windowMiddle+drawMiddle,drawHeight);
		glTexCoord2f(1,1); glVertex2f(windowMiddle+drawMiddle,0);
		glTexCoord2f(0,1); glVertex2f(windowMiddle-drawMiddle,0);
	glEnd();

	glutSwapBuffers();
}
