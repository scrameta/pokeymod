#include <conio.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <6502.h>

unsigned char * gtia_bgcol = (unsigned char *) 0xd01a;
unsigned char * pokeybase = (unsigned char *) 0xd200;
unsigned char * configbase = (unsigned char *) 0xd210;

void psgsound(unsigned char psgno, unsigned char channel, unsigned short freq, unsigned char vol)
{
	unsigned char * psg = pokeybase + 0xa0 + (psgno<<4);
	psg[channel<<1] = freq&0xff;
	psg[(channel<<1)+1] = freq>>8;
	psg[8+channel] = vol;
}

void configMode(unsigned char onoff)
{
	if (onoff)
		pokeybase[0xc] = 0x3f;
	else
		pokeybase[0xc] = 0x0;

}
enum {PSG_STMODE_MONO=0,PSG_STMODE_POLISH=1,PSG_STMODE_CZECH=2,PSG_STMODE_LR=3};
enum {PSG_FREQ_2MHZ=0,PSG_FREQ_1MHZ=1,PSG_FREQ_1_7MHZ=2};
enum {PSG_ENV_16=1,PSG_ENV_32};
enum {PSG_PROFILE_LOG1=0,PSG_PROFILE_LINEAR=3};

void psgSettings(unsigned char stereomode,unsigned char masterfreq,unsigned char env16,unsigned char profile)
{
	configMode(1);
	configbase[5] = (profile<<5)|(env16<<4)|(stereomode<<2)|(masterfreq);
	configMode(0);
}

void playEachPSGChannel()
{
	unsigned char ch;
	unsigned char psgno;

	printf("Play each PSG channel\n");

	for (psgno=0;psgno!=2;++psgno)
	{
		unsigned char * psg = pokeybase + 0xa0 + (psgno<<4);
		printf("psg:%x \n",psg);
		psg[7] = 0x38; // pure tone
		for (ch=0;ch!=3;++ch)
		{
			unsigned short freq;
			printf("%d/%d\n",psgno,ch);
			freq = ch;
			freq = freq<<6;
			freq = 10000-freq;
			psgsound(psgno,ch,freq,15);
			sleep(0.1);
			psgsound(psgno,ch,freq,14);
			sleep(0.1);
			psgsound(psgno,ch,freq,13);
			sleep(2);
			psgsound(psgno,ch,1000,0);
		}
	}
}

int main(void)
{
	unsigned char i=0;
	clrscr();
	printf("Pokeymax test: PSG\n");
	
	psgSettings(PSG_STMODE_LR,PSG_FREQ_2MHZ,PSG_ENV_16,PSG_PROFILE_LOG1);
	playEachPSGChannel();
	return 0;
}
	
