import os
import socket
import wave
import pyaudio
import imutils
import cv2
import numpy
import base64
import websockets
import asyncio
from pydub import AudioSegment

from pydub.utils import make_chunks
from aiohttp import web
import uuid
from TikTokApi import TikTokApi
import nest_asyncio
import random
import requests



host_ip = '192.168.86.38'
port = 8000

# This is a 'hack' because playwright already runs 
# in its own loop
nest_asyncio.apply()

video_que = asyncio.Queue()
audio_que = asyncio.Queue()

video_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "videos")
os.makedirs(video_path, exist_ok=True)


played_videos = set()
videos = []

def get_random_word():
    word_file = "/usr/share/dict/words"
    with open(word_file) as f:
        words = f.read().splitlines()
    item = random.randint(0, len(words)-1)
    word = words[item]
    return word


def get_video_set():

    '''
    READ FIRST:

    So the TikTok hashtag and trending
    feature appear to be broken now. It might
    start working in the future, but until then
    you will have to use option 2 or option 3 to
    get it working.

    Option 3 is basically adding the video ids that
    you want to a list
    '''

    
    ###################################
    # Option 1: This used to work.
    # with TikTokApi() as api:
    #     tag = api.hashtag(name=get_random_word())
    #     for video in tag.videos():
    #         videos.append(video.id)


    ###########################################
    # Option 2: This appears to work last time I tested it

    # https://github.com/avilash/TikTokAPI-Python
    # BASE_URL = 'https://www.tiktok.com/node/'

    # param = {
    #     "type": 5,
    #     "secUid": "",
    #     "id": '',
    #     "count": 30,
    #     "minCursor": 0,
    #     "maxCursor": 0,
    #     "shareUid": "",
    #     "lang": "",
    #     "verifyFp": "",
    # }
    # try:
    #     url = BASE_URL + 'video/feed'
    #     res = requests.get(
    #         url, 
    #         params=param,
    #         headers={
    #             "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.9",
    #             "authority": "www.tiktok.com",
    #             "Accept-Encoding": "gzip, deflate",
    #             "Connection": "keep-alive",
    #             "Host": "www.tiktok.com",
    #             "User-Agent": "Mozilla/5.0  (Windows NT 10.0; WOW64) AppleWebKit/537.36 (KHTML, like Gecko) coc_coc_browser/86.0.170 Chrome/80.0.3987.170 Safari/537.36",
    #         },
    #         cookies={}
    #     )
        
    #     resp = res.json()
    #     body = resp['body']
    #     for trend in body['itemListData']:
    #         print(trend['itemInfos']['text'])

    # except Exception:
    #     print(traceback.format_exc())
    #     return False

    ######################################

    # Option 3: Add the video IDs that you want to a list

    # trending_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),"trending_video_list")
    # with open(trending_path) as f:
    #     videos.extend(f.read().splitlines())
    

    


def get_audio_video_info():

    with TikTokApi() as api:
        item = random.randint(0, len(videos)-1)
        video_id = videos[item]

        video = api.video(id=video_id)
        video_bytes = video.bytes()

        filename = f"{uuid.uuid4()}.mp4"
        path = os.path.join(video_path, filename)

        with open(path, "wb") as out:
            out.write(video_bytes)

    # convert the video to 30 FPS. Output audio to seperate file
    command = "ffmpeg -y -i {} -filter:v fps=30 {} -filter:a 'volume=0.9' -ac 1 -ar 44100 {}".format(path, os.path.join(video_path, f"converted-{filename}"),'output.wav')
    os.system(command)
    items = [path, 'output.wav']
    return items
    

async def fill_buffers():
    max_video_lengths = []
    max_audio_lengths = []

    audio_bytes = []
    videoBytes = []


    items = get_audio_video_info()

    video = items[0]
    audio = items[1]


    myaudio = AudioSegment.from_file(audio, "wav")
    chunks = make_chunks(myaudio, (1/30)*1000)

    # process audio
    for i, chunk in enumerate(chunks):
        max_audio_lengths.append(len(chunk.raw_data))
        audio_bytes.append(bytearray(chunk.raw_data))

    vidcap = cv2.VideoCapture(video)
    while(vidcap.isOpened()):
        success, image = vidcap.read()
        if success:
            image = imutils.resize(image, width=135, height=240)
            buffer = cv2.imencode('.jpeg', image, [cv2.IMWRITE_JPEG_QUALITY, 50])[
                1].tobytes()
            video_frame = bytearray()
            frame_size = bytearray(
                len(buffer).to_bytes(2, 'big', signed=False))
            video_frame += frame_size
            video_frame_buffer = bytearray(buffer)
            video_frame += video_frame_buffer
            videoBytes.append(video_frame)
            max_video_lengths.append(len(buffer))
        else:
            break

    for i in range(len(videoBytes)):
        try:
            video = videoBytes[i]
            audio = audio_bytes[i]
            df = video + audio
            await video_que.put(bytes(df))
        except IndexError as err:
            print("out of range")
    print(
        f"Max frame size for video is {max(max_video_lengths)} with a total a total of {len(videoBytes)} frames")
    print(
        f"Max frame size for audio is {max(max_audio_lengths)} with a total a total of {len(audio_bytes)} frames")

    print("Buffers filled")





async def data_handler(websocket):
    while True:
        while not video_que.empty():
            video_frame = await video_que.get()
            await websocket.send(video_frame)
        await fill_buffers()
        await asyncio.sleep(5)
              


async def main():
    await asyncio.sleep(15)
    get_video_set()
    await fill_buffers()
    servver = await websockets.serve(data_handler, "0.0.0.0", 8080)
    await servver.wait_closed()


if __name__ == "__main__":
    asyncio.run(main())