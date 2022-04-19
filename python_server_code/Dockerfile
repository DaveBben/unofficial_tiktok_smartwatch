FROM mcr.microsoft.com/playwright:focal

WORKDIR /app

COPY requirements.txt requirements.txt

RUN apt-get update && apt install -y portaudio19-dev python3-pyaudio wamerican

RUN pip3 install -r requirements.txt

COPY . .
