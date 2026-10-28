import numpy as np, wave, glob, os
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

for f in ['wavetable','fm','harmonic','sample','granular','modal','resynth','organics','instrument','chop']:
    path=f'audio/{f}_demo.wav'
    if not os.path.exists(path): continue
    w=wave.open(path); n=w.getnframes(); sr=w.getframerate()
    d=np.frombuffer(w.readframes(n),dtype='<i2').astype(np.float32).reshape(-1,2)[:,0]/32768
    w.close()
    N=2048; hop=512
    frames=(len(d)-N)//hop
    S=np.zeros((N//2, frames), dtype=np.float32)
    win=np.hanning(N).astype(np.float32)
    for i in range(frames):
        seg=d[i*hop:i*hop+N]*win
        S[:,i]=np.abs(np.fft.rfft(seg))[:N//2]
    S=20*np.log10(np.maximum(S,1e-6)); S-=S.max()
    fig,ax=plt.subplots(figsize=(9,3.2),dpi=110)
    ax.imshow(S,origin='lower',aspect='auto',cmap='magma',vmin=-80,vmax=0,
              extent=[0,len(d)/sr,0,sr/2])
    ax.set_ylim(0,12000); ax.set_xlabel('время, с'); ax.set_ylabel('Гц')
    ax.set_title(f'OROGEN — {f}: демо-рендер (спектрограмма)')
    fig.tight_layout(); fig.savefig(f'audio/{f}_demo_spectrum.png'); plt.close(fig)
    print('ok', f)
