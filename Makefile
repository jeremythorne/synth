synth:
	g++ -std=c++20 main.cpp -g -O2 -I$$(brew --prefix)/include -L$$(brew --prefix)/lib -lSDL2 -o synth