// View ownership is explicit. Removing a display handle must not destroy shared assets.
export function withoutTextureOwnership(options){return options===true?{children:true}:options&&typeof options==='object'?{...options,texture:false,baseTexture:false,textureSource:false}:options}
export function destroyOwnedTextures(textures,options){
  if(!(typeof options==='boolean'?options:options?.texture))return
  const source=typeof options==='boolean'?options:options?.textureSource??options?.baseTexture
  for(const texture of new Set(textures))if(texture&&!texture.destroyed)texture.destroy(!!source)
}
