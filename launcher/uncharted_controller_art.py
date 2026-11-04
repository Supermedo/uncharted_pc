"""Original controller illustrations, drawn at high resolution like the launcher widgets."""
import math
from PIL import Image, ImageDraw, ImageFont, ImageChops

MODELS = [('auto', 'Automatic'), ('ps3', 'PlayStation 3 / DualShock 3'),
          ('playstation', 'PlayStation 4 / DualShock 4'), ('ps5', 'PlayStation 5 / DualSense'),
          ('xbox', 'Xbox / Series layout'), ('nintendo', 'Nintendo Switch Pro'),
          ('gamecube', 'Nintendo GameCube'),
          ('steam_deck', 'Steam Deck'), ('steam_controller', 'Steam Controller (2026)'),
          ('generic', 'Generic')]


class _ControllerArt:
    def __init__(self, width, background, accent):
        self.scale = width * 3 / 680
        self.image = Image.new('RGB', (round(680*self.scale), round(320*self.scale)), background)
        self.draw = ImageDraw.Draw(self.image)
        self.accent = accent
        self.edge, self.seam = '#9a9da3', '#454a52'
        self.shell, self.button = '#24272d', '#14171c'
        self.fonts = {}
        self.controls = {}

    def points(self, values):
        return [round(v*self.scale) for v in values]

    def line(self, points, color=None, width=1):
        self.draw.line(self.points(points), fill=color or self.seam,
                       width=max(1, round(width*self.scale)), joint='curve')

    def ellipse(self, box, fill=None, outline=None, width=1):
        self.draw.ellipse(self.points(box), fill=fill, outline=outline,
                          width=max(1, round(width*self.scale)))

    def rect(self, box, radius=0, fill=None, outline=None, width=1):
        self.draw.rounded_rectangle(self.points(box), radius=round(radius*self.scale),
                                   fill=fill, outline=outline,
                                   width=max(1, round(width*self.scale)))

    def text(self, x, y, text, size=10, color=None):
        pixels = max(1, round(size*self.scale))
        if pixels not in self.fonts:
            try:
                self.fonts[pixels] = ImageFont.truetype('segoeui.ttf', pixels)
            except OSError:
                self.fonts[pixels] = ImageFont.load_default(size=pixels)
        self.draw.text(tuple(self.points((x, y))), text, font=self.fonts[pixels],
                       anchor='mm', fill=color or '#e3e4e7')

    def curve(self, start, segments):
        points = [start]
        for x1, y1, x2, y2, x3, y3 in segments:
            x0, y0 = points[-1]
            for step in range(1, 25):
                t = step/24
                u = 1-t
                points.append((u**3*x0 + 3*u*u*t*x1 + 3*u*t*t*x2 + t**3*x3,
                               u**3*y0 + 3*u*u*t*y1 + 3*u*t*t*y2 + t**3*y3))
        return points

    def path(self, points, fill=None, outline=None, width=1):
        flat = [v for point in points for v in point]
        if fill:
            self.draw.polygon(self.points(flat), fill=fill)
        if outline:
            self.line(flat, outline, width)

    def body(self, start, right):
        half = self.curve(start, right)
        full = half + [(680-x, y) for x, y in reversed(half)]
        self.path(full, self.shell, self.edge, 1.5)

    def stick(self, x, y, radius=28):
        self.controls['leftstick' if x < 340 else 'rightstick'] = (x, y, radius)
        self.ellipse((x-radius-6, y-radius-6, x+radius+6, y+radius+6), '#1b1e24', self.seam)
        self.ellipse((x-radius, y-radius, x+radius, y+radius), '#33373e', self.edge, 1.2)
        self.ellipse((x-radius+5, y-radius+5, x+radius-5, y+radius-5), '#181b20', '#626771')
        self.ellipse((x-radius+8, y-radius+8, x+radius-8, y+radius-8), None, '#333840')
        for angle in range(0, 360, 15):
            a = math.radians(angle)
            self.line((x+(radius-2)*math.cos(a), y+(radius-2)*math.sin(a),
                       x+(radius-4)*math.cos(a), y+(radius-4)*math.sin(a)), '#727782', .6)

    def dpad(self, x, y, split=False, radius=29, dish=False):
        for name, dx, dy in (('dpup',0,-1),('dpdown',0,1),('dpleft',-1,0),('dpright',1,0)):
            self.controls[name] = (x+dx*radius*.7, y+dy*radius*.7, radius*.4)
        if split or dish:
            self.ellipse((x-radius-7, y-radius-7, x+radius+7, y+radius+7), '#202329', self.seam)
        if dish:
            self.ellipse((x-radius-3,y-radius-3,x+radius+3,y+radius+3), '#353941', '#858a92')
        if split:
            for turn in range(4):
                shape = [(-9, -29), (9, -29), (10, -15), (0, -6), (-10, -15), (-9, -29)]
                a = turn*math.pi/2
                points = [(x+dx*math.cos(a)-dy*math.sin(a), y+dx*math.sin(a)+dy*math.cos(a))
                          for dx, dy in shape]
                self.path(points, self.button, self.edge)
        else:
            r, b = radius, radius*.34
            pts = [(-b,-r),(b,-r),(b,-b),(r,-b),(r,b),(b,b),(b,r),(-b,r),
                   (-b,b),(-r,b),(-r,-b),(-b,-b),(-b,-r)]
            self.path([(x+dx,y+dy) for dx,dy in pts], self.button, self.edge)
        for turn in range(4):
            a = turn*math.pi/2
            points = [(-2,-radius*.67),(2,-radius*.67),(0,-radius*.78)]
            self.path([(x+dx*math.cos(a)-dy*math.sin(a), y+dx*math.sin(a)+dy*math.cos(a))
                       for dx,dy in points], fill='#959ba5')

    def face(self, x, y, labels, radius=15, distance=30, colors=None):
        for name, dx, dy in (('a',0,1),('b',1,0),('x',-1,0),('y',0,-1)):
            self.controls[name] = (x+dx*distance, y+dy*distance, radius)
        for dx, dy, label in ((0,distance,labels[0]),(distance,0,labels[1]),
                              (-distance,0,labels[2]),(0,-distance,labels[3])):
            cx, cy = x+dx, y+dy
            self.ellipse((cx-radius,cy-radius,cx+radius,cy+radius), self.button, '#7b8089', 1.1)
            self.ellipse((cx-radius+3,cy-radius+3,cx+radius-3,cy+radius-3), None, '#30353e', .6)
            if label == 'cross':
                self.line((cx-5,cy-5,cx+5,cy+5), '#afc6e1', 1.5)
                self.line((cx-5,cy+5,cx+5,cy-5), '#afc6e1', 1.5)
            elif label == 'circle':
                self.ellipse((cx-7,cy-7,cx+7,cy+7), None, '#e2a7aa', 1.4)
            elif label == 'square':
                self.rect((cx-6,cy-6,cx+6,cy+6), 1, outline='#d8b3da', width=1.4)
            elif label == 'triangle':
                self.path([(cx,cy-8),(cx+8,cy+6),(cx-8,cy+6),(cx,cy-8)], outline='#a7d3c7', width=1.4)
            else:
                self.text(cx, cy-1, label, 17, (colors or {}).get(label))

    def shoulders(self, left, right, x=220, y=36, width=72):
        self.controls.update(lefttrigger=(x,y+10,12), righttrigger=(680-x,y+10,12),
                             leftshoulder=(x,y+30,12), rightshoulder=(680-x,y+30,12))
        for center, names in ((x,left),(680-x,right)):
            self.rect((center-width/2+5,y,center+width/2-5,y+24), 9, '#181b21', self.edge)
            self.text(center,y+10,names[0],10,color='#c7cbd2')
            self.rect((center-width/2,y+21,center+width/2,y+40), 6, '#30343b', self.edge)
            self.text(center,y+30,names[1],10)

    def curved_shoulders(self, left, right, x, y, width):
        # Keep both labels above the shell, with a gap between the two rows.
        for center, names, side in ((x,left,'left'),(680-x,right,'right')):
            half = width/2
            trigger = self.curve((center-half+8,y+25), [
                (center-half+7,y+3,center+half-9,y-2,center+half-8,y+12),
                (center+half-7,y+17,center+half-7,y+22,center+half-7,y+27)])
            if side == 'right':
                trigger = [(2*center-px,py) for px,py in trigger]
            trigger = [(px,py-14) for px,py in trigger]
            self.path(trigger+[trigger[0]],self.button,self.edge,1.2)
            bumper = self.curve((center-half,y+48), [
                (center-half,y+23,center-half+8,y+22,center-4,y+22),
                (center+half-8,y+22,center+half,y+25,center+half,y+48)])
            if side == 'right':
                bumper = [(2*center-px,py) for px,py in bumper]
            bumper = [(px,py-8) for px,py in bumper]
            self.path(bumper+[bumper[0]],'#30343b',self.edge,1.2)
            self.text(center,y,names[0],9)
            self.text(center,y+25,names[1],9)
            self.controls[side+'trigger'] = (center,y,10)
            self.controls[side+'shoulder'] = (center,y+25,9)

    def small(self, x, y, label, radius=9):
        self.ellipse((x-radius,y-radius,x+radius,y+radius), self.button, '#626872')
        self.text(x,y-1,label,10)

    def playstation(self):
        self.controls.update(back=(254,111,10), start=(426,111,10), touchpad=(340,117,32))
        self.curved_shoulders(('L2','L1'),('R2','R1'),214,25,80)
        self.body((340,67), [(376,67,417,67,452,67),(485,67,501,84,514,114),
                            (530,153,544,205,547,248),(550,280,530,296,508,295),
                            (480,295,468,277,457,251),(450,234,443,217,434,218),
                            (413,241,389,232,366,230),(357,230,348,230,340,230)])
        for mirror in (False, True):
            pts = self.curve((166,163), [(153,200,143,247,156,268),(170,290,194,279,205,259),
                                        (216,240,223,224,237,214)])
            self.path([(680-x if mirror else x,y) for x,y in pts], outline=self.seam)
        self.rect((269,77,411,157), 9, '#181b21', '#7f858e', 1.2)
        self.line((282,81,398,81), self.accent, 1.7)
        for x in range(278,405,6):
            for y in range(92,151,6):
                self.ellipse((x,y,x+.7,y+.7), '#3d424b')
        self.rect((250,100,258,122),4,self.button,'#737a85')
        self.rect((422,100,430,122),4,self.button,'#737a85')
        self.text(254,89,'SHARE',7)
        self.text(426,89,'OPTIONS',7)
        for x in (213,467):
            self.ellipse((x-47,92,x+47,186),'#282c33','#575e68')
        self.dpad(213,139,True)
        self.face(467,139,('cross','circle','square','triangle'))
        self.stick(278,204)
        self.stick(402,204)
        for y in range(161,175,5):
            for x in range(326,355,7):
                self.ellipse((x,y,x+2,y+2),'#090b0e')
        self.small(340,202,'PS',11)
        self.rect((323,214,342,219),2,fill='#101217',outline=self.seam)
        self.ellipse((348,213,354,219),'#101217',self.seam)

    def xbox(self):
        self.controls.update(back=(310,154,10), start=(370,154,10), misc1=(340,176,10))
        self.curved_shoulders(('LT','LB'),('RT','RB'),222,21,92)
        self.body((340,67), [(368,67,390,67,407,67),(419,67,423,55,435,55),
                            (458,56,484,65,497,80),(516,106,529,143,539,184),
                            (548,222,553,260,539,279),(526,298,510,294,493,279),
                            (475,262,455,240,436,229),(418,223,374,225,340,225)])
        for flip in (False,True):
            pts=self.curve((183,80),[(202,74,220,70,245,75),(267,80,270,112,293,117),
                                    (308,122,324,121,340,121)])
            self.path([(680-x if flip else x,y) for x,y in pts],outline='#535963')
            pts=self.curve((160,175),[(148,219,146,251,162,267),(176,280,196,257,216,238)])
            self.path([(680-x if flip else x,y) for x,y in pts],outline=self.seam)
        self.stick(222,143,29)
        self.face(467,140,('A','B','X','Y'), colors={
            'A':'#9fca82', 'B':'#e59595', 'X':'#86bcea', 'Y':'#e5d282'})
        self.dpad(285,194,radius=24,dish=True)
        self.stick(400,194,27)
        self.small(340,94,'',16)
        for flip in (False,True):
            pts=self.curve((329,83),[(336,84,345,92,351,104),(348,94,341,84,329,83)])
            self.path([(680-x if flip else x,y) for x,y in pts],fill='#d8dce2')
        self.small(310,154,'')
        self.rect((306,150,312,155),1,outline='#a5aab2')
        self.rect((309,153,315,158),1,outline='#a5aab2')
        self.small(370,154,'')
        for y in (150,154,158): self.line((366,y,374,y),'#a5aab2')
        self.rect((329,169,351,183),6,self.button,'#626872')
        self.line((334,176,334,179,346,179,346,176),'#b4bac4')
        self.line((340,177,340,172,337,175,340,172,343,175),'#b4bac4')

    def nintendo(self):
        self.controls.update(back=(290,100,10), start=(390,100,10), misc1=(309,151,10))
        self.curved_shoulders(('ZL','L'),('ZR','R'),225,23,84)
        self.body((340,66),[(380,66,419,66,455,66),(483,66,500,83,507,109),
                           (518,152,533,211,540,248),(548,282,527,297,508,296),
                           (484,296,470,276,456,250),(450,239,444,234,438,234),
                           (413,234,373,234,340,234)])
        for flip in (False,True):
            pts=self.curve((170,157),[(164,181,153,225,148,248),(141,277,159,290,177,285),
                                    (198,276,221,239,242,234),(219,210,191,178,170,157)])
            self.path([(680-x if flip else x,y) for x,y in pts],fill='#1c1f25',outline='#525861')
        self.stick(224,133,28)
        self.face(461,133,('B','A','Y','X'),14,29)
        self.dpad(279,196,radius=23)
        self.stick(398,196,28)
        self.small(290,100,'−',10)
        self.small(390,100,'+',10)
        self.rect((302,144,316,158),3,self.button,'#686e78')
        self.ellipse((306,148,312,154),None,'#9ba0aa')
        self.small(371,151,'',9)
        self.path([(367,152),(371,148),(375,152)],outline='#a4aab3')
        self.rect((368,152,374,156),0,outline='#a4aab3')
        for x in (330,337,344,351): self.rect((x,225,x+3,227),1,fill='#626b77')

    def gamecube(self):
        # SDL's GameCube order is A, X, B, Y (south, east, west, north).
        self.shell = '#49406c'
        for x,label in ((218,'L'),(462,'R')):
            self.rect((x-35,20,x+35,46),12,'#9b9da7',self.edge)
            self.text(x,32,label,12,'#181b20')
        self.rect((480,48,521,64),7,'#646297',self.edge)
        self.text(500,56,'Z',10)
        # Some third-party variants add a small left shoulder button.
        self.rect((159,48,200,64),7,'#646297',self.edge)
        self.text(180,56,'L1',10)
        self.body((340,65),[(385,66,451,73,488,94),(520,111,528,140,530,181),
                           (534,223,536,276,520,297),(508,314,490,303,481,282),
                           (471,260,466,233,458,216),(451,227,447,250,423,258),
                           (389,271,367,249,370,220),(373,201,387,185,392,177),
                           (376,173,355,173,340,173)])
        for flip in (False,True):
            pts=self.curve((165,159),[(190,169,207,186,223,201),(231,213,231,241,254,249),
                                     (280,259,303,245,307,228)])
            self.path([(680-x if flip else x,y) for x,y in pts],outline='#655b87')
        self.ellipse((174,79,283,181),'#3c345b','#584b7c')
        self.ellipse((402,82,516,178),'#3c345b','#584b7c')
        for x,y,r,color,edge in ((229,128,29,'#b9bac4','#e0e1e7'),
                                (407,218,23,'#dfb842','#ffe58c')):
            pts=[(x+(r+9)*math.cos(math.pi/8+i*math.pi/4),
                  y+(r+9)*math.sin(math.pi/8+i*math.pi/4)) for i in range(9)]
            self.path(pts,'#272539','#766b93',1.2)
            self.ellipse((x-r,y-r,x+r,y+r),color,edge,1.2)
            for inset in (5,9,13):
                self.ellipse((x-r+inset,y-r+inset,x+r-inset,y+r-inset),None,
                             '#9293a0' if x<340 else '#c89d31',.7)
        self.text(407,218,'C',16,'#725b1c')
        self.dpad(273,218,radius=22,dish=True)
        self.ellipse((436,108,486,158),'#329b79','#8ad4b3',1.2)
        self.text(461,133,'A',22,'#eafff5')
        self.ellipse((411,157,441,187),'#b53e49','#ec8b90',1.2)
        self.text(426,172,'B',15)
        for label,cx,cy,angle in (('Y',447,93,-20),('X',500,132,80)):
            a=math.radians(angle)
            pts=[(cx+22*math.cos(t)*math.cos(a)-11*math.sin(t)*math.sin(a),
                  cy+22*math.cos(t)*math.sin(a)+11*math.sin(t)*math.cos(a))
                 for t in [i*math.tau/48 for i in range(49)]]
            self.path(pts,'#a6a9b2','#e0e2e8',1.2)
            self.text(cx,cy,label,13,'#252830')
        self.ellipse((329,106,351,128),'#a6a9b2','#e0e2e8')
        self.text(340,144,'START / PAUSE',7)
        self.controls.update(a=(461,133,25),b=(500,132,14),x=(426,172,15),y=(447,93,14),
                             start=(340,117,12),lefttrigger=(218,32,14),righttrigger=(462,32,14),
                             leftshoulder=(180,56,12),
                             rightshoulder=(500,56,12))

    def steam(self):
        self.controls.update(back=(114,67,9), start=(566,67,9), misc1=(538,223,10),
                             leftshoulder=(112,38,10), rightshoulder=(568,38,10),
                             lefttrigger=(112,13,10), righttrigger=(568,13,10))
        self.rect((89,2,135,25),9,self.button,self.edge)
        self.rect((545,2,591,25),9,self.button,self.edge)
        self.text(112,13,'L2',9)
        self.text(568,13,'R2',9)
        self.rect((73,28,153,48),8,'#15181e',self.edge)
        self.rect((527,28,607,48),8,'#15181e',self.edge)
        self.text(112,38,'L1',9)
        self.text(568,38,'R1',9)
        self.body((340,52),[(432,52,512,50,572,50),(609,50,630,70,630,105),
                           (630,148,630,203,628,232),(626,262,608,278,571,278),
                           (506,278,420,276,340,276)])
        self.rect((176,62,504,266),7,'#101318','#484e58')
        # The LCD's active area is 16:10; keep the bezel outside those proportions.
        self.rect((192,72,488,257),3,'#080b10','#282f38')
        self.line((202,77,478,77),'#303b48',.7)
        for flip in (False,True):
            pts=self.curve((51,145),[(70,145,87,142,87,159),(87,204,105,233,141,274)])
            self.path([(680-x if flip else x,y) for x,y in pts],outline=self.seam)
        self.stick(142,105,23)
        self.stick(538,105,23)
        self.dpad(85,96,radius=18)
        self.face(595,96,('A','B','X','Y'),11,22)
        for x in (105,515):
            self.rect((x,144,x+60,204),8,'#181c23','#7c8490',1.1)
            self.rect((x+3,147,x+57,201),6,None,'#303741')
        self.rect((106,63,122,72),4,self.button,'#69717d')
        self.rect((111,65,116,69),0,outline='#b2bac5')
        self.rect((558,63,574,72),4,self.button,'#69717d')
        for y in (65,67,69): self.line((563,y,569,y),'#b2bac5')
        self.rect((122,216,162,231),7,self.button,'#717a87')
        self.text(142,223,'STEAM',7)
        self.rect((518,216,558,231),7,self.button,'#717a87')
        self.text(538,220,'...',15)
        for x in (129,523):
            for y in range(242,253,4):
                for dx in (0,7,14,21): self.rect((x+dx,y,x+dx+5,y+1),.5,fill='#667180')
        for x in (228,452): self.ellipse((x,57,x+2,59),'#0a0c10','#626b77')

    def generic(self):
        self.controls.update(back=(315,132,10), start=(365,132,10))
        self.shoulders(('L2','L1'),('R2','R1'),221,29,75)
        self.body((340,76),[(390,76,426,65,464,77),(502,89,511,122,517,164),
                           (528,222,551,269,521,283),(491,299,460,253,436,231),
                           (411,211,373,219,340,219)])
        self.dpad(218,143,radius=27)
        self.face(462,143,('A','B','X','Y'))
        self.stick(278,210,27)
        self.stick(402,210,27)
        self.rect((304,126,327,138),6,self.button,'#777e88')
        self.rect((353,126,376,138),6,self.button,'#777e88')
        self.line((337,91,343,91),self.accent,2)

    def ps3(self):
        self.curved_shoulders(('L2','L1'),('R2','R1'),216,25,74)
        self.body((340,88),[(369,88,391,88,410,88),(428,68,469,62,490,83),
                           (514,105,520,139,520,175),(520,208,528,260,512,278),
                           (498,294,470,294,454,277),(439,260,435,231,417,214),
                           (397,210,369,214,340,214)])
        for x in (216,464): self.ellipse((x-55,77,x+55,187),'#282c33','#626872',1.2)
        self.dpad(216,132,True)
        self.face(464,132,('cross','circle','square','triangle'))
        for x in (281,399):
            self.controls['leftstick' if x < 340 else 'rightstick'] = (x,198,29)
            self.ellipse((x-37,161,x+37,235),self.shell,'#646b75',1.2)
            self.ellipse((x-29,169,x+29,227),'#30343b',self.edge,1.2)
            self.ellipse((x-24,174,x+24,222),'#24282e','#565d67')
        self.rect((306,120,320,128),1,self.button,self.edge)
        self.path([(360,120),(373,125),(360,130),(360,120)],self.button,self.edge)
        self.text(313,111,'SELECT',7)
        self.text(366,111,'START',7)
        self.small(340,158,'PS',12)
        self.rect((330,177,350,183),2,self.button,self.seam)
        self.controls.update(back=(313,124,9),start=(366,125,9))

    def ps5(self):
        self.curved_shoulders(('L2','L1'),('R2','R1'),215,20,83)
        self.body((340,54),[(383,54,420,52,461,64),(490,73,505,100,516,135),
                           (534,186,546,243,536,273),(532,289,519,298,505,294),
                           (480,282,465,246,448,221),(438,207,426,210,403,210),
                           (381,210,359,210,340,210)])
        for flip in (False,True):
            pts=self.curve((267,62),[(276,110,278,147,258,176),(232,208,210,250,179,287)])
            self.path([(680-x if flip else x,y) for x,y in pts],outline='#a4a9b3',width=1.5)
        touchpad=self.curve((340,62),[(367,62,391,62,403,65),(412,66,413,71,411,80),
                                    (408,100,403,122,397,135),(394,141,387,142,378,142),
                                    (365,142,352,142,340,142)])
        self.path(touchpad+[(680-x,y) for x,y in reversed(touchpad)],'#181b21','#858c99',1.4)
        for flip in (False,True):
            pts=self.curve((264,68),[(268,86,273,112,279,130)])
            self.path([(680-x if flip else x,y) for x,y in pts],outline='#a5bcd5',width=1.5)
        self.dpad(213,128,True)
        self.face(467,128,('cross','circle','square','triangle'))
        self.stick(278,195,27)
        self.stick(402,195,27)
        self.rect((249,83,256,103),3,self.button,self.edge)
        self.rect((424,83,431,103),3,self.button,self.edge)
        for x in range(326,355,7):
            for y in (153,158): self.ellipse((x,y,x+2,y+2),'#0b0d11')
        self.small(340,176,'PS',10)
        self.rect((332,194,348,198),2,self.button,self.edge)
        self.controls.update(back=(252,93,9), start=(427,93,9),touchpad=(340,110,32),misc1=(340,196,9))

    def steam_controller_shell(self):
        self.body((340,61),[(385,61,429,59,465,63),(496,67,510,86,520,121),
                           (534,164,544,224,541,261),(541,286,529,301,507,300),
                           (486,300,479,287,471,269),(465,253,459,250,446,250),
                           (414,250,375,250,340,250)])

    def steam_controller(self):
        self.curved_shoulders(('L2','L1'),('R2','R1'),220,20,82)
        self.steam_controller_shell()
        for flip in (False,True):
            panel=self.curve((340,153),[(302,153,253,151,224,157),(199,164,192,193,188,227),
                                       (184,249,181,272,178,291)])
            self.path([(680-x if flip else x,y) for x,y in panel],outline=self.seam)
            pad=self.curve((237,166),[(231,165,224,169,223,176),(220,188,216,210,217,221),
                                     (217,229,222,233,230,234),(246,237,279,241,290,241),
                                     (298,241,302,235,303,228),(305,214,309,194,309,186),
                                     (309,178,304,175,297,174),(282,171,249,166,237,166)])
            self.path([(680-x if flip else x,y) for x,y in pad],'#181c23','#89919c',1.3)
        self.dpad(214,112,radius=25)
        self.face(467,112,('A','B','X','Y'),13,29)
        self.stick(282,139,25)
        self.stick(398,139,25)
        self.small(340,115,'',13)
        self.ellipse((338,82,342,86),'#c1c8d0')
        self.rect((265,79,289,91),6,self.button,self.edge)
        self.rect((391,79,415,91),6,self.button,self.edge)
        self.rect((272,82,279,86),1,outline='#a5aab2')
        self.rect((275,84,282,88),1,outline='#a5aab2')
        for y in (82,85,88): self.line((399,y,407,y),'#a5aab2')
        self.rect((326,201,354,214),6,self.button,self.edge)
        for x in (334,340,346): self.ellipse((x-1,206,x+1,208),'#a5aab2')
        self.controls.update(back=(277,85,9),start=(403,85,9),misc1=(340,207,9))


def controller_art(profile, width=680, background='#17181b', accent='#e7c36a'):
    """Image and logical hit targets share the same drawing coordinates."""
    art = _ControllerArt(width, background, accent)
    painters = {'playstation': art.playstation, 'xbox': art.xbox, 'nintendo': art.nintendo,
                'gamecube': art.gamecube,
                'steam': art.steam, 'steam_deck': art.steam, 'steam_controller': art.steam_controller,
                'ps3': art.ps3, 'ps5': art.ps5, 'generic': art.generic}
    painters.get(profile, art.generic)()
    return art.image.resize((width, round(width*320/680)), Image.Resampling.LANCZOS), art.controls


def render_controller(profile, width=680, background='#17181b', accent='#e7c36a'):
    return controller_art(profile,width,background,accent)[0]


def fitted_controller_art(model, width, rear=False, background='#17181b', accent='#e7c36a'):
    """Crop unused margins and transform the drawing and hit regions together."""
    renderer = controller_rear_art if rear else controller_art
    image,controls=renderer(model,680,background,accent)
    box=ImageChops.difference(image,Image.new('RGB',image.size,background)).getbbox()
    left,top,right,bottom=box or (0,0,*image.size)
    left,top=max(0,left-10),max(0,top-10)
    right,bottom=min(image.width,right+10),min(image.height,bottom+10)
    ratio=width/(right-left)
    image=image.crop((left,top,right,bottom)).resize((width,round((bottom-top)*ratio)),Image.Resampling.LANCZOS)
    return image,{key:((x-left)*ratio,(y-top)*ratio,r*ratio) for key,(x,y,r) in controls.items()}


def controller_rear_art(model, width=300, background='#17181b', accent='#e7c36a'):
    """Rear grips use physical hand labels: the player's right appears on the left."""
    art = _ControllerArt(width,background,accent)
    deck = model in ('steam_deck','steam')
    if deck:
        art.rect((60,45,170,72),12,art.button,art.edge)
        art.rect((510,45,620,72),12,art.button,art.edge)
        art.body((340,61),[(420,61,522,57,578,61),(620,65,636,85,636,128),
                           (636,186,640,253,605,274),(574,289,438,278,340,278)])
        for x in range(318,423,8): art.line((x,94,x,153),'#111419',3)
        for x in (179,501):
            art.line((x,77,x,260),art.seam)
        art.rect((272,200,414,231),4,None,art.seam)
        art.line((289,214,397,214),art.seam)
        left,right,top,bottom = 130,550,137,216
    else:
        if model == 'steam_controller':
            art.curved_shoulders(('R2','R1'),('L2','L1'),220,20,82)
            art.steam_controller_shell()
        else:
            art.shoulders(('R2','R1'),('L2','L1'),219,25,86)
            art.body((340,67),[(395,67,429,56,466,69),(501,83,513,130,524,177),
                           (539,239,533,280,506,284),(483,287,461,245,434,236),
                           (413,229,374,233,340,233)])
        art.rect((286,90,394,205),16,'#20242a',art.seam)
        art.line((300,112,380,112),art.seam)
        left,right,top,bottom = 241,439,142,211
    controls = {}
    for name,x,y,label in (('paddle1',left,top,'R4'),('paddle2',right,top,'L4'),
                            ('paddle3',left,bottom,'R5'),('paddle4',right,bottom,'L5')):
        art.rect((x-22,y-23,x+22,y+23),10,'#343a44','#a0a8b4',1.4)
        art.line((x-13,y+14,x+13,y+14),'#5a6574')
        art.text(x,y,label,13)
        controls[name]=(x,y,23)
    for x,y in ((192,83),(488,83),(192,251),(488,251)):
        art.ellipse((x-3,y-3,x+3,y+3),'#101319','#515965')
    return art.image.resize((width,round(width*320/680)),Image.Resampling.LANCZOS),controls
