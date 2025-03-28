FROM ubuntu:20.04
RUN apt-get update -y
RUN apt-get install wget build-essential unzip -y
WORKDIR /opt/
RUN wget https://github.com/PRCYCoin/PRCYCoin/releases/download/2.0.0.6/prcycoin-v2.0.0.6-x86_64-linux.zip
RUN unzip prcycoin-v2.0.0.6-x86_64-linux.zip
RUN chmod +x prcycoind
RUN chmod +x prcycoin-cli
RUN mv prcycoind /usr/bin
RUN mv prcycoin-cli /usr/bin
RUN rm prcycoin*
RUN wget https://raw.githubusercontent.com/TheRetroMike/rmt-nomp/master/scripts/blocknotify.c
RUN gcc blocknotify.c -o /usr/bin/blocknotify
CMD /usr/bin/prcycoind -printtoconsole
