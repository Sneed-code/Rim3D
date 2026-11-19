# Makes the kill-confirm sound (cues/Ui_KillConfirm/1-3.wav): a low punch, a click and a rising two-note metal ding.
# Usage: python3 tools/sounds/kill_confirm.py game/Content/Rim3D/Sounds/cues/Ui_KillConfirm
import numpy as np, wave, sys
SR=48000
def env(t,a,d): return np.minimum(t/a,1.0)*np.exp(-t/d)
def make(seed, base, out):
    rng=np.random.default_rng(seed)
    n=int(SR*0.55); t=np.arange(n)/SR
    s=np.zeros(n)
    # low punch: sine with fast pitch drop
    f=55+110*np.exp(-t/0.03)
    ph=2*np.pi*np.cumsum(f)/SR
    s+=0.85*np.sin(ph)*env(t,0.002,0.07)
    # click transient (bandpassed noise)
    nz=rng.standard_normal(n)
    hp=np.diff(np.concatenate([[0],nz]))
    s+=0.35*hp*env(t,0.0005,0.006)
    # metallic ding 1 (inharmonic partials), slightly delayed
    def ding(f0,t0,amp,dec):
        tt=np.clip(t-t0,0,None); on=(t>=t0)
        y=np.zeros(n)
        for k,(r,a) in enumerate([(1,1.0),(2.01,0.45),(2.76,0.35),(4.07,0.18),(5.4,0.1)]):
            y+=a*np.sin(2*np.pi*f0*r*tt+rng.uniform(0,6.28))*np.exp(-tt/(dec/(1+0.6*k)))
        return amp*y*on*np.minimum(tt/0.001,1)
    s+=ding(base,0.008,0.28,0.22)
    s+=ding(base*1.335,0.075,0.32,0.30)   # rising fourth: "confirm"
    # gentle saturation and fade
    s=np.tanh(1.6*s)/np.tanh(1.6)
    s*=np.minimum(1,(t[-1]-t)/0.05)
    s/=np.max(np.abs(s)); s*=0.89
    pcm=(s*32767).astype('<i2')
    w=wave.open(out,'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes(pcm.tobytes()); w.close()
d=sys.argv[1]
for i,(seed,base) in enumerate([(1,1320.0),(2,1290.0),(3,1350.0)]):
    make(seed,base,f"{d}/{i+1}.wav")
