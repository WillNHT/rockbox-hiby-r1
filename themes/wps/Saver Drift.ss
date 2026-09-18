#
# Saver Drift - a screensaver with a clip of its own.
#
# %Cf plays a file from this skin's folder (Saver Drift/drift.webp) on a
# loop: slow colour, then the time. Any GIF, WebP or video can take its
# place. See docs/mods/video.md.
#
%wd
# No backdrop: the ground is black.
%X(d)
%Fl(2,24-GeistMono-SemiBold.fnt)
%Fl(5,58-GeistMono-SemiBold.fnt)
#
%V(0,0,-,-,1)
%Vb(000000)
#
%V(0,0,480,640,-)
%Cf(0,0,480,640,drift.webp,cover)
#
%V(0,660,-,64,5)
%Vb(000000)
%Vf(F4F2EE)
%ac%cH:%cM
#
%V(30,730,-30,30,2)
%Vb(000000)
%Vf(8E8C86)
%ac%s%?it<%it - %?ia<%ia|>|%fn>
