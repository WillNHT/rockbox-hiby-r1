#
# Saver Canvas - a screensaver for the charger.
#
# The playing track's music video or animated cover, large, with the time
# under it; the cover when the track has neither. %Cv is the same picture
# the WPS shows, so a music video carries on from where it was. See
# docs/mods/video.md.
#
%wd
# No backdrop: the ground is black.
%X(d)
%Fl(2,24-GeistMono-SemiBold.fnt)
%Fl(5,58-GeistMono-SemiBold.fnt)
%?CV<%Vd(vid)|%Vd(vid)|%Vd(art)>
#
%V(0,0,-,-,1)
%Vb(000000)
#
%Vl(vid,0,110,480,480,-)
%Vb(000000)
%Cv(0,0,480,480,cover)
#
%Vl(art,0,110,480,480,-)
%Vb(000000)
%Cl(0,0,480,480,c,c)
%Cd
#
%V(0,620,-,64,5)
%Vb(000000)
%Vf(F4F2EE)
%ac%cH:%cM
#
%V(30,696,-30,60,2)
%Vb(000000)
%Vf(8E8C86)
%ac%s%?it<%it|%fn>
%ac%s%?ia<%ia|>
