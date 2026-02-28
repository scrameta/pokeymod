#include <conio.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <6502.h>

unsigned char * gtia_bgcol = (unsigned char *) 0xd01a;
unsigned char * pokeybase = (unsigned char *) 0xd200;
unsigned char * configbase = (unsigned char *) 0xd210;

unsigned char * sidbase1 = (unsigned char *) 0xd240;

void sidsoundoff(unsigned char sidno, unsigned char channel);
void sidset(unsigned char sidno, unsigned char channel,unsigned char reg, unsigned char val)
{
        unsigned char volatile * sid1 = sidbase1 + (sidno<<5);
        unsigned char volatile * voice1;

       	if (channel == 1)
	{
		voice1 = sid1+7;
	}
	else if (channel == 2)
	{
		voice1 = sid1+14;
	}
	else
	{
		voice1 = sid1;
	}
	voice1[reg] = val;
}
void sidinit(unsigned char sidno)
{
	unsigned channel;
	for (channel=0;channel!=3;++channel)
	{
		sidset(sidno,channel,4,8);
	}
	for (channel=0;channel!=3;++channel)
	{
		sidset(sidno,channel,4,0);
	}
	sleep(1);
	for (channel=0;channel!=3;++channel)
	{
		sidsoundoff(sidno,channel);
	}
}

void sidnoise(unsigned char sidno, unsigned char channel, unsigned short freq, unsigned char vol)
{
	unsigned char * sid = pokeybase + 0x40 + (sidno<<5);
	unsigned char * voice = sid;
	sid[24] = vol; // not per channel!
	if (channel == 1)
		voice = voice+7;
	if (channel == 2)
		voice = voice+14;
	voice[0] = freq&0xff;
	voice[1] = freq>>8;
	voice[2] = 0x0;
	voice[3] = 8;
	voice[6] = 0xff; // sustain at 100%
	voice[4] = 0x81; // gate
}

enum {SID_TRIANGLE=0x10,SID_SAWTOOTH=0x20,SID_PULSE=0x40,SID_NOISE=0x80,SID_RINGMOD=4,SID_SYNC=2};

void sidsound(unsigned char sidno, unsigned char channel, unsigned short freq, unsigned short wave)
{
	sidset(sidno,channel,0,freq&0xff);
	sidset(sidno,channel,1,freq>>8);
	if (wave==SID_PULSE)
	{
		sidset(sidno,channel,2,0);
		sidset(sidno,channel,3,8);
	}
	else
	{
		sidset(sidno,channel,2,0x00);
		sidset(sidno,channel,3,0x00);
	}
	sidset(sidno,channel,5,0x88);
	sidset(sidno,channel,6,0x36); 
/*	if ((wave)&SID_SAWTOOTH)
		printf("S");
	if ((wave)&SID_PULSE)
		printf("P");
	if ((wave)&SID_TRIANGLE)
		printf("T");
	if ((wave)&SID_NOISE)
		printf("N");*/
	sidset(sidno,channel,4,0x1 | wave); // gate
	sleep(1);
	sidset(sidno,channel,4,0x0 | wave); // gate
}

void sidsoundoff(unsigned char sidno, unsigned char channel)
{
	sidset(sidno,channel,6,0x00); // sustain at 0%
	sidset(sidno,channel,4,0x10); // gate off
}

int main(void)
{
	unsigned char i=0;
	clrscr();
	cprintf("Pokeymax test: SID waves");
	
	sidsound(0,0,1000,15);

        sidbase1[23] = 0x00;
        sidbase1[24] = 0x0f;

        sidbase1[23+32] = 0x00;
        sidbase1[24+32] = 0x0f;

	while (1)
	{
		int wave;
		int sidno;
		int freq;
		for (sidno=0;sidno!=2;++sidno)
		{
			for (wave=1;wave!=9;++wave)
			{
				for (freq=1000;freq!=3000;freq+=500)
				{
					printf("sid:%d wave:%d freq:%d\n",sidno,wave,freq);
					sidsound(sidno,0,freq,wave<<4);
					sidsoundoff(sidno,0);
					if (kbhit())
						return 0;
				}
			}
		}
	}

	return 0;
}

