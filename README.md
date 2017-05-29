# fixGB
Hi there, this is my little GB and GBC Emulator project, its essentially the GB version of my NES Emulator, fixNES.  
If you want to check it out for some reason I do include a windows binary in the "Releases" tab, if you want to compile it go check out the "build" files.  
You will need freeglut as well as openal-soft to compile the project, it should run on most systems since it is fairly generic C code.  
Right now GB and GBC titles using MBC1, 2, 3, 5 and HuC1 should work just fine and also save into standard .sav files.  
You can also listen to .gbs files, changing tracks works by pressing left/right.  
To load a file, just drag and drop the .gb/.gbc/.gbs file into the application or call it via command line like "fixGB your_rom.gb".    

Controls right now are keyboard only and do the following:  
Y/Z is A  
X is B  
A is Start  
S is select  
Arrow Keys is DPad  
Keys 1-9 integer-scale the window to number  
P is Pause  
If you really want controller support and you are on windows, go grab joy2key, it works just fine with fixGB (and fixNES).    

That is all I can say about it right now, who knows if I will write some more on it.  