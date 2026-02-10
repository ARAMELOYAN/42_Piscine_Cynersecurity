#!/bin/bash

sudo apt update
sudo apt install -y g++ gcc libexiv2-dev make libcurl4-openssl-dev pkg-config
g++ -std=c++17 -O2 -Wall -Wextra scorpion.cpp -lexiv2 -o scorpion_cpp
gcc -std=c17 -O2 -Wall -Wextra scorpion.c $(pkg-config --cflags --libs gexiv2) -o scorpion_c                                                                                                                                  
gcc -std=c17 -O2 -Wall -Wextra spider.c -lcurl -o spider_c
g++ -std=c++17 -O2 -Wall -Wextra spider.cpp -lcurl -o spider_cpp