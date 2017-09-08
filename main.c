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
#include <ctype.h>
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
#if ZIPSUPPORT
#include "unzip/unzip.h"
#endif
#define DEBUG_HZ 0
#define DEBUG_MAIN_CALLS 0
#define DEBUG_KEY 0
#define DEBUG_LOAD_INFO 1

static const char *VERSION_STRING = "fixGB Alpha v0.8";
static char window_title[256];
static char window_title_pause[256];

enum {
	FTYPE_UNK = 0,
	FTYPE_GB,
	FTYPE_GBC,
	FTYPE_GBS,
#if ZIPSUPPORT
	FTYPE_ZIP,
#endif
};

static void gbEmuFileOpen(char *name);
static bool gbEmuFileRead();
static void gbEmuFileClose();

static void gbEmuDisplayFrame(void);
static void gbEmuMainLoop(void);
static void gbEmuDeinit(void);

static void gbEmuHandleKeyDown(unsigned char key, int x, int y);
static void gbEmuHandleKeyUp(unsigned char key, int x, int y);
static void gbEmuHandleSpecialDown(int key, int x, int y);
static void gbEmuHandleSpecialUp(int key, int x, int y);

static int emuFileType = FTYPE_UNK;
static char emuFileName[1024];
uint8_t *emuGBROM = NULL;
uint32_t emuGBROMsize = 0;
char emuSaveName[1024];
//used externally
uint32_t textureImage[0x5A00];
bool gbPause = false;
bool gbEmuGBSPlayback = false;
bool gbsTimerMode = false;
uint16_t gbsLoadAddr = 0;
uint16_t gbsInitAddr = 0;
uint16_t gbsPlayAddr = 0;
uint32_t gbsRomSize = 0;
uint16_t gbsSP = 0;
uint8_t gbsTracksTotal = 0, gbsTMA = 0, gbsTAC = 0;
uint8_t cpuTimer = 3;
bool gbCgbGame = false;
bool gbCgbMode = false;
bool gbCgbBootrom = false;
bool gbAllowInvVRAM = false;

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

static uint32_t linesToDraw = VISIBLE_LINES;
static const uint32_t visibleImg = VISIBLE_DOTS*VISIBLE_LINES*4;
static uint8_t scaleFactor = 3;
static uint32_t mainLoopRuns;
static uint16_t mainLoopPos;
//from input.c
extern uint8_t inValReads[8];

int main(int argc, char** argv)
{
	puts(VERSION_STRING);
	strcpy(window_title, VERSION_STRING);
	memset(textureImage,0,visibleImg);
	emuFileType = FTYPE_UNK;
	memset(emuFileName,0,1024);
	memset(emuSaveName,0,1024);
	if(argc >= 2)
		gbEmuFileOpen(argv[1]);
	if(emuFileType == FTYPE_GB || emuFileType == FTYPE_GBC)
	{
		if(!gbEmuFileRead())
		{
			gbEmuFileClose();
			printf("Main: Could not read %s!\n", emuFileName);
			puts("Press enter to exit");
			getc(stdin);
			return EXIT_SUCCESS;
		}
		gbEmuFileClose();
		memcpy(emuSaveName, emuFileName, 1024);
		if(emuFileType == FTYPE_GBC)
			memcpy(emuSaveName+strlen(emuSaveName)-3,"sav",3);
		else //.gb has one less character
			memcpy(emuSaveName+strlen(emuSaveName)-2,"sav",3);
		printf("Save Path: %s\n",emuSaveName); 
		//Set Invalid VRAM allowed
		gbAllowInvVRAM = (strstr(emuFileName,"InvVRAM") != NULL);
		printf("Main: Invalid VRAM Access is %sallowed\n", gbAllowInvVRAM?"":"dis");
		gbCgbBootrom = memInitCGBBootrom();
		//Verify Header CRC
		uint8_t hdrcrc = 0;
		uint16_t hdrpos;
		for(hdrpos = 0x134; hdrpos < 0x14D; hdrpos++)
			hdrcrc = hdrcrc-emuGBROM[hdrpos]-1;
		if(hdrcrc != emuGBROM[0x14D])
		{
			printf("Main: WARNING: Invalid ROM Header CRC, ROM may not work\n");
			if(gbCgbBootrom)
			{
				//Fix Header CRC for Bootrom
				emuGBROM[0x14D] = hdrcrc;
			}
		}
		//Set CGB Regs allowed
		gbCgbGame = (emuGBROM[0x143] == 0x80 || emuGBROM[0x143] == 0xC0);
		gbCgbMode = (gbCgbGame || gbCgbBootrom);
		printf("Main: CGB Regs are %sallowed\n", gbCgbMode?"":"dis");
		if(!memInit(true,false))
		{
			free(emuGBROM);
			puts("Press enter to exit");
			getc(stdin);
			return EXIT_SUCCESS;
		}
		//CPU DMG Mode
		cpuSetSpeed(false);
		apuInitBufs();
		cpuInit();
		ppuInit();
		apuInit();
		inputInit();
		if(emuGBROM[0x134] != 0)
		{
			if(gbCgbMode)
			{
				printf("Game: %.11s\n", (char*)(emuGBROM+0x134));
				sprintf(window_title, "%.11s (CGB) - %s\n", (char*)(emuGBROM+0x134), VERSION_STRING);
			}
			else
			{
				printf("Game: %.16s\n", (char*)(emuGBROM+0x134));
				sprintf(window_title, "%.16s (DMG) - %s\n", (char*)(emuGBROM+0x134), VERSION_STRING);
			}
		}
	}
	else if(emuFileType == FTYPE_GBS)
	{
		if(!gbEmuFileRead())
		{
			gbEmuFileClose();
			printf("Main: Could not read %s!\n", emuFileName);
			puts("Press enter to exit");
			getc(stdin);
			return EXIT_SUCCESS;
		}
		gbEmuFileClose();
		uint8_t *tmpROM = emuGBROM;
		uint32_t tmpROMsize = emuGBROMsize;
		gbsTracksTotal = tmpROM[4];
		gbsLoadAddr = (tmpROM[6])|(tmpROM[7]<<8);
		gbsInitAddr = (tmpROM[8])|(tmpROM[9]<<8);
		gbsPlayAddr = (tmpROM[0xA])|(tmpROM[0xB]<<8);
		gbsSP = (tmpROM[0xC])|(tmpROM[0xD]<<8);
		//should give more than enough room for everything
		gbsRomSize = emuGBROMsize = (tmpROMsize-0x70+gbsLoadAddr+0x7FFF)&(~0x7FFF);
		//printf("Main: gbsLoadAddr %04x gbsInitAddr %04x gbsPlayAddr %04x gbsSP %04x\n",
		//	gbsLoadAddr, gbsInitAddr, gbsPlayAddr, gbsSP);
		emuGBROM = malloc(emuGBROMsize);
		memset(emuGBROM,0xFF,emuGBROMsize);
		memcpy(emuGBROM+gbsLoadAddr,tmpROM+0x70,tmpROMsize-0x70);
		gbsTMA = tmpROM[0xE];
		gbsTAC = tmpROM[0xF];
		if(gbsTAC&0x80)
		{
			cpuSetSpeed(true);
			gbCgbGame = gbCgbMode = true;
		}
		else
		{
			cpuSetSpeed(false);
			gbCgbGame = gbCgbMode = false;
		}
		printf("Main: CGB Regs are %sallowed\n", gbCgbMode?"":"dis");
		if(gbsTAC&4)
		{
			printf("Main: GBS Play Timing: Timer\n");
			gbsTimerMode = true;
		}
		else
		{
			printf("Main: GBS Play Timing: VSync\n");
			gbsTimerMode = false;
		}
		memInit(true,true);
		if(tmpROM[0x10] != 0)
		{
			printf("Game: %.32s\n",(char*)(tmpROM+0x10));
			sprintf(window_title, "%.32s (GBS) - %s\n", (char*)(tmpROM+0x10), VERSION_STRING);
		}
		free(tmpROM);
		apuInitBufs();
		//does all inits for us
		memStartGBS();
		gbEmuGBSPlayback = true;
		linesToDraw = 20;
		scaleFactor = 4;
	}
	if(emuGBROM == NULL)
	{
#if ZIPSUPPORT
		printf("Main: No File to Open! Make sure to call fixGB with a .gb/.gbc/.gbs/.zip File as Argument.\n");
#else
		printf("Main: No File to Open! Make sure to call fixGB with a .gb/.gbc/.gbs File as Argument.\n");
#endif
		puts("Press enter to exit");
		getc(stdin);
		return EXIT_SUCCESS;
	}
	sprintf(window_title_pause, "%s (Pause)", window_title);
	#if WINDOWS_BUILD
	#if DEBUG_HZ
	emuFrameStart = GetTickCount();
	#endif
	#if DEBUG_MAIN_CALLS
	emuMainFrameStart = GetTickCount();
	#endif
	#endif
	//do one scanline per idle loop
	mainLoopRuns = 70224;
	mainLoopPos = mainLoopRuns;
	glutInit(&argc, argv);
	glutInitWindowSize(VISIBLE_DOTS*scaleFactor, linesToDraw*scaleFactor);
	glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGBA);
	glutCreateWindow(gbPause ? window_title_pause : window_title);
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
	glTexImage2D(GL_TEXTURE_2D, 0, 4, VISIBLE_DOTS, linesToDraw, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, textureImage);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glEnable(GL_TEXTURE_2D);
	glShadeModel(GL_FLAT);

	glutMainLoop();

	return EXIT_SUCCESS;
}

static FILE *gbEmuFilePointer = NULL;
#if ZIPSUPPORT
static bool gbEmuFileIsZip = false;
static uint8_t *gbEmuZipBuf = NULL;
static uint32_t gbEmuZipLen = 0;
static unzFile gbEmuZipObj;
static unz_file_info gbEmuZipObjInfo;
#endif
static int gbEmuGetFileType(char *name)
{
	int nLen = strlen(name);
	if(nLen > 4 && name[nLen-4] == '.')
	{
		if(tolower(name[nLen-3]) == 'g' && tolower(name[nLen-2]) == 'b' && tolower(name[nLen-1]) == 'c')
			return FTYPE_GBC;
		else if(tolower(name[nLen-3]) == 'g' && tolower(name[nLen-2]) == 'b' && tolower(name[nLen-1]) == 's')
			return FTYPE_GBS;
#if ZIPSUPPORT
		else if(tolower(name[nLen-3]) == 'z' && tolower(name[nLen-2]) == 'i' && tolower(name[nLen-1]) == 'p')
			return FTYPE_ZIP;
#endif
	}
	else if(nLen > 3 && name[nLen-3] == '.')
	{
		if(tolower(name[nLen-2]) == 'g' && tolower(name[nLen-1]) == 'b')
			return FTYPE_GB;
	}
	return FTYPE_UNK;
}

static void gbEmuFileOpen(char *name)
{
	emuFileType = FTYPE_UNK;
	memset(emuFileName,0,1024);
	memset(emuSaveName,0,1024);
	int baseType = gbEmuGetFileType(name);
#if ZIPSUPPORT
	if(baseType == FTYPE_ZIP)
	{
		printf("Base ZIP File: %s\n", name);
		FILE *tmp = fopen(name,"rb");
		if(!tmp)
		{
			printf("Main: Could not open %s!\n", name);
			return;
		}
		fseek(tmp,0,SEEK_END);
		gbEmuZipLen = ftell(tmp);
		rewind(tmp);
		gbEmuZipBuf = malloc(gbEmuZipLen);
		if(!gbEmuZipBuf)
		{
			printf("Main: Could not allocate ZIP buffer!\n");
			fclose(tmp);
			return;
		}
		fread(gbEmuZipBuf,1,gbEmuZipLen,tmp);
		fclose(tmp);
		char filepath[20];
		snprintf(filepath,20,"%x+%x",(unsigned int)gbEmuZipBuf,gbEmuZipLen);
		gbEmuZipObj = unzOpen(filepath);
		int err = unzGoToFirstFile(gbEmuZipObj);
		while (err == UNZ_OK)
		{
			char tmpName[256];
			err = unzGetCurrentFileInfo(gbEmuZipObj,&gbEmuZipObjInfo,tmpName,256,NULL,0,NULL,0);
			if(err == UNZ_OK)
			{
				int curInZipType = gbEmuGetFileType(tmpName);
				if(curInZipType != FTYPE_ZIP && curInZipType != FTYPE_UNK)
				{
					emuFileType = curInZipType;
					gbEmuFileIsZip = true;
					if(strchr(name,'/') != NULL || strchr(name,'\\') != NULL)
					{
						char *nPath = name;
						if(strchr(nPath,'/') != NULL)
							nPath = (strrchr(nPath,'/')+1);
						if(strchr(nPath,'\\') != NULL)
							nPath = (strrchr(nPath,'\\')+1);
						strncpy(emuFileName, name, nPath-name);
					}
					char *zName = tmpName;
					if(strchr(zName,'/') != NULL)
						zName = (strrchr(zName,'/')+1);
					if(strchr(zName,'\\') != NULL)
						zName = (strrchr(zName,'\\')+1);
					strcat(emuFileName, zName);
					printf("File in ZIP Type: %s\n", emuFileType == FTYPE_GB ? "GB" : (emuFileType == FTYPE_GBC ? "GBC" : "GBS"));
					printf("Full Path from ZIP: %s\n", emuFileName);
					break;
				}
				else
					err = unzGoToNextFile(gbEmuZipObj);
			}
		}
		if(emuFileType == FTYPE_UNK)
		{
			printf("Found no usable file in ZIP\n");
			unzClose(gbEmuZipObj);
			if(gbEmuZipBuf)
				free(gbEmuZipBuf);
			gbEmuZipBuf = NULL;
			gbEmuZipLen = 0;
		}
	}
	else if(baseType != FTYPE_UNK)
#else
	if(baseType != FTYPE_UNK)
#endif
	{
		gbEmuFilePointer = fopen(name,"rb");
		if(!gbEmuFilePointer)
			printf("Main: Could not open %s!\n", name);
		else
		{
			emuFileType = baseType;
			strncpy(emuFileName, name, 1024);
			printf("File Type: %s\n", baseType == FTYPE_GB ? "GB" : (baseType == FTYPE_GBC ? "GBC" : "GBS"));
			printf("Full Path: %s\n", emuFileName);
		}
	}
}

static bool gbEmuFileRead()
{
#if ZIPSUPPORT
	if(gbEmuFileIsZip)
	{
		unzOpenCurrentFile(gbEmuZipObj);
		emuGBROMsize = gbEmuZipObjInfo.uncompressed_size;
		emuGBROM = malloc(emuGBROMsize);
		if(emuGBROM)
			unzReadCurrentFile(gbEmuZipObj,emuGBROM,emuGBROMsize);
		unzCloseCurrentFile(gbEmuZipObj);
	}
	else
#endif
	{
		fseek(gbEmuFilePointer,0,SEEK_END);
		emuGBROMsize = ftell(gbEmuFilePointer);
		rewind(gbEmuFilePointer);
		emuGBROM = malloc(emuGBROMsize);
		if(emuGBROM)
			fread(emuGBROM,1,emuGBROMsize,gbEmuFilePointer);
	}
	if(emuGBROM)
		return true;
	//else
	printf("Main: Could not allocate ROM buffer!\n");
	return false;
}

//cleans up vars from read
static void gbEmuFileClose()
{
#if ZIPSUPPORT
	if(gbEmuFileIsZip)
		unzClose(gbEmuZipObj);
	gbEmuFileIsZip = false;
	if(gbEmuZipBuf)
		free(gbEmuZipBuf);
	gbEmuZipBuf = NULL;
	gbEmuZipLen = 0;
#endif
	if(gbEmuFilePointer)
		fclose(gbEmuFilePointer);
	gbEmuFilePointer = NULL;
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
		if((!emuSkipVsync && emuRenderFrame) || gbPause)
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
			cpuCycle();
			//mem clock tied to CPU clock, so
			//double speed in CGB mode!
			if(!(memClock&3))
				memClockTimers();
			memClock++;
		}
		//run PPU last
		ppuCycle();
		if(ppuDrawDone())
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
			//send VSync to GBS Player if required
			if(gbEmuGBSPlayback && !gbsTimerMode)
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
				gbPause ^= true;
				glutSetWindowTitle(gbPause ? window_title_pause : window_title);
			}
			break;
		case '1':
			if(!inResize)
			{
				inResize = true;
				glutReshapeWindow(VISIBLE_DOTS*1, linesToDraw*1);
			}
			break;
		case '2':
			if(!inResize)
			{
				inResize = true;
				glutReshapeWindow(VISIBLE_DOTS*2, linesToDraw*2);
			}
			break;
		case '3':
			if(!inResize)
			{
				inResize = true;
				glutReshapeWindow(VISIBLE_DOTS*3, linesToDraw*3);
			}
			break;
		case '4':
			if(!inResize)
			{
				inResize = true;
				glutReshapeWindow(VISIBLE_DOTS*4, linesToDraw*4);
			}
			break;
		case '5':
			if(!inResize)
			{
				inResize = true;
				glutReshapeWindow(VISIBLE_DOTS*5, linesToDraw*5);
			}
			break;
		case '6':
			if(!inResize)
			{
				inResize = true;
				glutReshapeWindow(VISIBLE_DOTS*6, linesToDraw*6);
			}
			break;
		case '7':
			if(!inResize)
			{
				inResize = true;
				glutReshapeWindow(VISIBLE_DOTS*7, linesToDraw*7);
			}
			break;
		case '8':
			if(!inResize)
			{
				inResize = true;
				glutReshapeWindow(VISIBLE_DOTS*8, linesToDraw*8);
			}
			break;
		case '9':
			if(!inResize)
			{
				inResize = true;
				glutReshapeWindow(VISIBLE_DOTS*9, linesToDraw*9);
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
		glTexImage2D(GL_TEXTURE_2D, 0, 4, VISIBLE_DOTS, linesToDraw, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, textureImage);
		emuRenderFrame = false;
	}

	glClear(GL_COLOR_BUFFER_BIT);

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, glutGet(GLUT_WINDOW_WIDTH), 0, glutGet(GLUT_WINDOW_HEIGHT), -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	double upscaleVal = round((((double)glutGet(GLUT_WINDOW_HEIGHT))/((double)linesToDraw))*20.0)/20.0;
	double windowMiddle = ((double)glutGet(GLUT_WINDOW_WIDTH))/2.0;
	double drawMiddle = (((double)VISIBLE_DOTS)*upscaleVal)/2.0;
	double drawHeight = ((double)linesToDraw)*upscaleVal;

	glBegin(GL_QUADS);
		glTexCoord2f(0,0); glVertex2f(windowMiddle-drawMiddle,drawHeight);
		glTexCoord2f(1,0); glVertex2f(windowMiddle+drawMiddle,drawHeight);
		glTexCoord2f(1,1); glVertex2f(windowMiddle+drawMiddle,0);
		glTexCoord2f(0,1); glVertex2f(windowMiddle-drawMiddle,0);
	glEnd();

	glutSwapBuffers();
}
