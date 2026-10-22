// A portable Pixi 8 example with no external services.
globalThis.PixiNativeApp={
  start:async function(context){
    var P=context.PIXI;
    var app=new P.Application();
    await app.init({width:innerWidth,height:innerHeight,background:0x101a2e,resolution:devicePixelRatio});
    var title=new P.Text({text:'Hello Pixi Native',style:{fontFamily:'sans-serif',fontSize:42,fill:0xffffff}});
    title.position.set(48,36);app.stage.addChild(title);
    var subtitle=new P.Text({text:'Independent runtime / tap the card',style:{fontFamily:'sans-serif',fontSize:22,fill:0x9eacc7}});
    subtitle.position.set(50,96);app.stage.addChild(subtitle);
    var sprite=P.Sprite.from(await P.Assets.load('assets/card.png'));
    sprite.anchor.set(0.5);sprite.position.set(innerWidth/2,innerHeight/2);
    sprite.eventMode='static';sprite.cursor='pointer';app.stage.addChild(sprite);
    var clicks=Number(localStorage.getItem('hello.clicks')||0);
    var status=new P.Text({text:'Taps: '+clicks,style:{fontFamily:'sans-serif',fontSize:24,fill:0x5de0c8}});
    status.position.set(50,innerHeight-65);app.stage.addChild(status);
    sprite.on('pointertap',function(){
      clicks++;localStorage.setItem('hello.clicks',String(clicks));status.text='Taps: '+clicks;
    });
    var clock=0;
    app.ticker.add(function(delta){clock+=delta.deltaTime/60;sprite.rotation=Math.sin(clock)*0.12;});
    addEventListener('resize',function(){
      sprite.position.set(innerWidth/2,innerHeight/2);status.y=innerHeight-65;
    });

    return app;
  }
};
