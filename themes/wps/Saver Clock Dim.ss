#
# Saver Clock Dim - a screensaver for the battery.
#
# The time and what is playing, in grey on black, and nothing else to
# draw. The clock moves to a new place every half minute, so the picture
# is never quite the same. Written like a WPS; see docs/mods/video.md.
#
%wd
# No backdrop: the ground is black.
%X(d)
%Fl(2,24-GeistMono-SemiBold.fnt)
%Fl(5,58-GeistMono-SemiBold.fnt)
#
# Four places, half a minute each.
%?if(%an(4,30000),=,1)<%Vd(c1)%Vd(t1)>
%?if(%an(4,30000),=,2)<%Vd(c2)%Vd(t2)>
%?if(%an(4,30000),=,3)<%Vd(c3)%Vd(t3)>
%?if(%an(4,30000),=,4)<%Vd(c4)%Vd(t4)>
#
# The ground.
%V(0,0,-,-,1)
%Vb(000000)
%Vf(000000)
#
%Vl(c1,0,180,-,64,5)
%Vb(000000)
%Vf(8A8A8A)
%ac%cH:%cM
%Vl(t1,30,250,-30,60,2)
%Vb(000000)
%Vf(5A5A5A)
%ac%s%?it<%it|%fn>
%ac%s%?ia<%ia|>
#
%Vl(c2,0,330,-,64,5)
%Vb(000000)
%Vf(8A8A8A)
%ac%cH:%cM
%Vl(t2,30,400,-30,60,2)
%Vb(000000)
%Vf(5A5A5A)
%ac%s%?it<%it|%fn>
%ac%s%?ia<%ia|>
#
%Vl(c3,0,480,-,64,5)
%Vb(000000)
%Vf(8A8A8A)
%ac%cH:%cM
%Vl(t3,30,550,-30,60,2)
%Vb(000000)
%Vf(5A5A5A)
%ac%s%?it<%it|%fn>
%ac%s%?ia<%ia|>
#
%Vl(c4,0,260,-,64,5)
%Vb(000000)
%Vf(8A8A8A)
%ac%cH:%cM
%Vl(t4,30,330,-30,60,2)
%Vb(000000)
%Vf(5A5A5A)
%ac%s%?it<%it|%fn>
%ac%s%?ia<%ia|>
