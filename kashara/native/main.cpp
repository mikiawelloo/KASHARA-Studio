// SPDX-License-Identifier: GPL-2.0-or-later
#include <obs.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <QApplication>
#include <QMainWindow>
#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QListWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QComboBox>
#include <QLineEdit>
#include <QInputDialog>
#include <QDialog>
#include <QMessageBox>
#include <QFileDialog>
#include <QDir>
#include <QFileInfo>
#include <QTimer>
#include <QSlider>
#include <QCheckBox>
#include <QStandardPaths>
#include <QDesktopServices>
#include <QUrl>
#include <QShowEvent>
#include <QResizeEvent>
#include <QPaintEngine>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QFile>
#include <QSignalBlocker>
#include <algorithm>
#include <vector>
#include <memory>

static QByteArray utf(const QString &s) { return s.toUtf8(); }
static QString str(const char *s) { return QString::fromUtf8(s ? s : ""); }
static QString configDir() {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}
class Preview : public QWidget {
    obs_display_t *display = nullptr;
    static void draw(void *, uint32_t w, uint32_t h) {
        obs_video_info v{};
        if (!obs_get_video_info(&v) || !w || !h) return;
        double scale = std::min(double(w)/v.base_width,double(h)/v.base_height);
        int x = int((w-v.base_width*scale)/2), y = int((h-v.base_height*scale)/2);
        gs_viewport_push(); gs_projection_push();
        gs_set_viewport(x,y,int(v.base_width*scale),int(v.base_height*scale));
        gs_ortho(0,float(v.base_width),0,float(v.base_height),-100,100);
        obs_render_main_texture();
        gs_projection_pop(); gs_viewport_pop();
    }
public:
    Preview() { setAttribute(Qt::WA_NativeWindow); setAttribute(Qt::WA_PaintOnScreen); setAttribute(Qt::WA_NoSystemBackground); setMinimumSize(320,180); }
    QPaintEngine *paintEngine() const override { return nullptr; }
    void shutdown() { if(display){obs_display_remove_draw_callback(display,draw,this);obs_display_destroy(display);display=nullptr;} }
    ~Preview() override { shutdown(); }
protected:
    void showEvent(QShowEvent *e) override {
        QWidget::showEvent(e);
        if(display) return;
        gs_init_data d{}; d.window.hwnd=reinterpret_cast<void*>(winId()); d.cx=uint32_t(width()*devicePixelRatioF()); d.cy=uint32_t(height()*devicePixelRatioF()); d.num_backbuffers=1; d.format=GS_BGRA; d.zsformat=GS_ZS_NONE;
        display=obs_display_create(&d,0x101010);
        if(display) obs_display_add_draw_callback(display,draw,this);
    }
    void resizeEvent(QResizeEvent *e) override { QWidget::resizeEvent(e); if(display) obs_display_resize(display,uint32_t(width()*devicePixelRatioF()),uint32_t(height()*devicePixelRatioF())); }
};
class Studio : public QMainWindow {
    Preview *preview; QListWidget *scenes,*sources; QLabel *status; QLineEdit *server,*key; QPushButton *live,*record; QSlider *volume; QCheckBox *mute;
    std::vector<obs_scene_t*> sceneRefs;
    std::vector<obs_source_t*> restored;
    obs_output_t *stream=nullptr,*rec=nullptr; obs_service_t *service=nullptr;
    obs_encoder_t *video=nullptr,*audio=nullptr,*recVideo=nullptr,*recAudio=nullptr;
    QTimer *timer=nullptr;
    bool streamPending=false,recordPending=false; int streamTicks=0,recTicks=0;
    QString projectFile= configDir()+"/project.json";
    obs_scene_t *scene() const { int i=scenes->currentRow();return i>=0 && i<int(sceneRefs.size())?sceneRefs[i]:nullptr; }
    obs_sceneitem_t *item() const { auto *i=sources->currentItem();return scene()&&i?obs_scene_find_sceneitem_by_id(scene(),i->data(Qt::UserRole).toLongLong()):nullptr; }
    void message(const QString &s){status->setText(s);}
    void error(const QString &s){message(s);QMessageBox::warning(this,"KASHARA Studio",s);}
    template<class F> QPushButton *button(QLayout *l,const QString &s,F fn){auto *b=new QPushButton(s);l->addWidget(b);connect(b,&QPushButton::clicked,this,fn);return b;}
    QWidget *panel(QBoxLayout *row,const QString &title,QVBoxLayout **layout,int stretch=0){auto *p=new QWidget; p->setObjectName("panel");auto *l=new QVBoxLayout(p);auto *t=new QLabel(title);t->setObjectName("heading");l->addWidget(t);row->addWidget(p,stretch);*layout=l;return p;}
    QString unique(const QString &base){QString n=base;int i=2;while(auto *s=obs_get_source_by_name(utf(n).constData())){obs_source_release(s);n=base+" "+QString::number(i++);}return n;}
    void addScene(const QString &name){auto *s=obs_scene_create(utf(unique(name)).constData());if(!s)return;sceneRefs.push_back(s);scenes->addItem(str(obs_source_get_name(obs_scene_get_source(s))));scenes->setCurrentRow(scenes->count()-1);}
    void refresh(){sources->clear();if(!scene())return;obs_scene_enum_items(scene(),[](obs_scene_t*,obs_sceneitem_t *i,void *p){auto *list=static_cast<QListWidget*>(p);auto *s=obs_sceneitem_get_source(i);auto *r=new QListWidgetItem(str(obs_source_get_name(s)),list);r->setData(Qt::UserRole,qlonglong(obs_sceneitem_get_id(i)));r->setFlags(r->flags()|Qt::ItemIsUserCheckable);r->setCheckState(obs_sceneitem_visible(i)?Qt::Checked:Qt::Unchecked);return true;},sources);if(sources->count())sources->setCurrentRow(sources->count()-1);}
    void audioUI(){auto *i=item();auto *s=i?obs_sceneitem_get_source(i):nullptr;bool ok=s&&(obs_source_get_output_flags(s)&OBS_SOURCE_AUDIO);volume->setEnabled(ok);mute->setEnabled(ok);QSignalBlocker a(volume),b(mute);volume->setValue(ok?int(obs_source_get_volume(s)*100):100);mute->setChecked(ok&&obs_source_muted(s));}
    void save(){if(sceneRefs.empty())return;auto *d=obs_data_create();auto *a=obs_save_sources();obs_data_set_array(d,"sources",a);obs_data_array_release(a);obs_data_set_string(d,"current",scene()?obs_source_get_name(obs_scene_get_source(scene())):"");obs_data_save_json_safe(d,utf(projectFile).constData(),"tmp","bak");obs_data_release(d);}
    void restore(){auto *d=obs_data_create_from_json_file(utf(projectFile).constData());if(!d)return;auto *a=obs_data_get_array(d,"sources");QString current=str(obs_data_get_string(d,"current"));if(a){obs_load_sources(a,[](void *p,obs_source_t *s){auto *self=static_cast<Studio*>(p);self->restored.push_back(obs_source_get_ref(s));if(auto *sc=obs_scene_from_source(s)){self->sceneRefs.push_back(obs_scene_get_ref(sc));self->scenes->addItem(str(obs_source_get_name(s)));}},this);obs_data_array_release(a);}obs_data_release(d);for(int i=0;i<scenes->count();++i)if(scenes->item(i)->text()==current){scenes->setCurrentRow(i);return;}if(scenes->count())scenes->setCurrentRow(0);}
    void addSource(const QString &id,const QString &label,obs_data_t *settings=nullptr){if(!scene()){error("Create a scene first.");return;}const char *latest=obs_get_latest_input_type_id(utf(id).constData());auto *s=obs_source_create(latest?latest:utf(id).constData(),utf(unique(label)).constData(),settings,nullptr);if(!s){error("Source unavailable: "+id);return;}auto *i=obs_scene_add(scene(),s);if(i&&(obs_source_get_output_flags(s)&OBS_SOURCE_VIDEO)){vec2 bounds;vec2_set(&bounds,1920,1080);obs_sceneitem_set_bounds_type(i,OBS_BOUNDS_SCALE_INNER);obs_sceneitem_set_bounds(i,&bounds);}obs_source_release(s);refresh();save();if(i) message("Added "+label);}
    void addWeb(bool demo){QString address=demo?QDir(QCoreApplication::applicationDirPath()+"/../../widgets").absoluteFilePath("demo.html"):QInputDialog::getText(this,"Widget","HTTPS URL");if(address.isEmpty())return;if(!demo&&(QUrl(address).scheme()!="https"||QUrl(address).host().isEmpty())){error("Enter an HTTPS URL.");return;}if(demo&&!QFileInfo::exists(address)){error("Demo widget file missing.");return;}auto *d=obs_data_create();obs_data_set_bool(d,"is_local_file",demo);obs_data_set_string(d,demo?"local_file":"url",utf(address).constData());obs_data_set_int(d,"width",1920);obs_data_set_int(d,"height",1080);addSource("browser_source",demo?"Demo widget":"Web widget",d);obs_data_release(d);}
    void addMedia(bool image){QString path=QFileDialog::getOpenFileName(this,image?"Image":"Media",{},image?"Images (*.png *.jpg *.jpeg *.webp)":"Media (*.mp4 *.mkv *.mov *.webm *.mp3 *.wav)");if(path.isEmpty())return;auto *d=obs_data_create();obs_data_set_string(d,image?"file":"local_file",utf(path).constData());if(!image)obs_data_set_bool(d,"is_local_file",true);addSource(image?"image_source":"ffmpeg_source",QFileInfo(path).fileName(),d);obs_data_release(d);}
    void properties(){auto *i=item();if(!i)return;auto *s=obs_sceneitem_get_source(i);auto *props=obs_source_properties(s);if(!props){error("No settings for this source.");return;}QDialog dlg(this);dlg.setWindowTitle(str(obs_source_get_name(s)));auto *form=new QFormLayout(&dlg);auto *d=obs_source_get_settings(s);auto *p=obs_properties_first(props);for(;p;obs_property_next(&p)){if(!obs_property_visible(p)||!obs_property_enabled(p))continue;QString name=str(obs_property_name(p));QString label=str(obs_property_description(p));auto type=obs_property_get_type(p);if(type==OBS_PROPERTY_LIST&&obs_property_list_format(p)==OBS_COMBO_FORMAT_STRING){auto *c=new QComboBox;for(size_t j=0;j<obs_property_list_item_count(p);++j)c->addItem(str(obs_property_list_item_name(p,j)),str(obs_property_list_item_string(p,j)));int n=c->findData(str(obs_data_get_string(d,utf(name).constData())));if(n>=0)c->setCurrentIndex(n);form->addRow(label,c);connect(c,&QComboBox::currentIndexChanged,&dlg,[d,name,c](int){obs_data_set_string(d,utf(name).constData(),utf(c->currentData().toString()).constData());});}else if(type==OBS_PROPERTY_BOOL){auto *c=new QCheckBox;c->setChecked(obs_data_get_bool(d,utf(name).constData()));form->addRow(label,c);connect(c,&QCheckBox::toggled,&dlg,[d,name](bool v){obs_data_set_bool(d,utf(name).constData(),v);});}else if(type==OBS_PROPERTY_TEXT||type==OBS_PROPERTY_PATH){auto *e=new QLineEdit(str(obs_data_get_string(d,utf(name).constData())));form->addRow(label,e);connect(e,&QLineEdit::textChanged,&dlg,[d,name](const QString &v){obs_data_set_string(d,utf(name).constData(),utf(v).constData());});}}button(form,"Apply",[&]{dlg.accept();});if(dlg.exec()==QDialog::Accepted){obs_source_update(s,d);save();}obs_data_release(d);obs_properties_destroy(props);}
    bool encoders(obs_encoder_t *&v,obs_encoder_t *&a,const char *name,int bitrate){auto *d=obs_data_create();obs_data_set_int(d,"bitrate",bitrate);obs_data_set_string(d,"rate_control","CBR");obs_data_set_string(d,"preset","veryfast");obs_data_set_int(d,"keyint_sec",2);v=obs_video_encoder_create("obs_x264",name,d,nullptr);obs_data_release(d);d=obs_data_create();obs_data_set_int(d,"bitrate",160);a=obs_audio_encoder_create("ffmpeg_aac",name,d,0,nullptr);obs_data_release(d);if(!v||!a)return false;obs_encoder_set_video(v,obs_get_video());obs_encoder_set_audio(a,obs_get_audio());return true;}
    void releaseStream(){if(stream){obs_output_release(stream);stream=nullptr;}if(service){obs_service_release(service);service=nullptr;}if(video){obs_encoder_release(video);video=nullptr;}if(audio){obs_encoder_release(audio);audio=nullptr;}streamPending=false;live->setText("GO LIVE");}
    void releaseRecord(){if(rec){obs_output_release(rec);rec=nullptr;}if(recVideo){obs_encoder_release(recVideo);recVideo=nullptr;}if(recAudio){obs_encoder_release(recAudio);recAudio=nullptr;}recordPending=false;record->setText("RECORD");}
    void startStream(){if(stream){obs_output_stop(stream);message("Stopping stream...");return;}if(key->text().isEmpty()){error("Enter a YouTube stream key.");return;}auto *d=obs_data_create();obs_data_set_string(d,"server",utf(server->text()).constData());obs_data_set_string(d,"key",utf(key->text()).constData());service=obs_service_create("rtmp_custom","YouTube",d,nullptr);obs_data_release(d);stream=obs_output_create("rtmp_output","KASHARA live",nullptr,nullptr);if(!stream||!service||!encoders(video,audio,"live",6000)){releaseStream();error("Streaming modules or encoders unavailable.");return;}obs_output_set_service(stream,service);obs_output_set_video_encoder(stream,video);obs_output_set_audio_encoder(stream,audio,0);if(!obs_output_start(stream)){QString e=str(obs_output_get_last_error(stream));releaseStream();error("Stream failed: "+e);return;}streamPending=true;streamTicks=0;live->setText("STOP LIVE");message("Connecting to YouTube...");}
    void startRecord(){if(rec){obs_output_stop(rec);message("Stopping recording...");return;}QString path=QFileDialog::getSaveFileName(this,"Save recording",QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)+"/KASHARA-"+QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss")+".mkv","Matroska (*.mkv)");if(path.isEmpty())return;auto *d=obs_data_create();obs_data_set_string(d,"path",utf(path).constData());rec=obs_output_create("ffmpeg_muxer","KASHARA recording",d,nullptr);obs_data_release(d);if(!rec||!encoders(recVideo,recAudio,"record",10000)){releaseRecord();error("Recording modules unavailable.");return;}obs_output_set_video_encoder(rec,recVideo);obs_output_set_audio_encoder(rec,recAudio,0);if(!obs_output_start(rec)){QString e=str(obs_output_get_last_error(rec));releaseRecord();error("Recording failed: "+e);return;}recordPending=true;recTicks=0;record->setText("STOP RECORD");message("Starting recording...");}
    void tick(){if(stream){if(obs_output_active(stream)){streamPending=false;message("LIVE | "+QString::number(obs_output_get_total_frames(stream))+" frames | dropped "+QString::number(obs_output_get_frames_dropped(stream)));}else if(!streamPending||++streamTicks>60){auto e=str(obs_output_get_last_error(stream));releaseStream();message(e.isEmpty()?"Stream stopped":e);}}if(rec){if(obs_output_active(rec)){recordPending=false;if(!stream)message("Recording");}else if(!recordPending||++recTicks>60){auto e=str(obs_output_get_last_error(rec));releaseRecord();message(e.isEmpty()?"Recording stopped":e);}}}
public:
    Studio(){setWindowTitle("KASHARA Studio 1.0 Alpha");resize(1400,850);auto *root=new QWidget;setCentralWidget(root);auto *v=new QVBoxLayout(root);auto *header=new QHBoxLayout;v->addLayout(header);auto *brand=new QLabel("KASHARA  /  STUDIO");brand->setObjectName("brand");header->addWidget(brand);header->addStretch();button(header,"YOUTUBE STUDIO",[]{QDesktopServices::openUrl(QUrl("https://studio.youtube.com/"));});auto *r=new QHBoxLayout;v->addLayout(r,1);QVBoxLayout *left,*middle,*right;panel(r,"SCENES / SOURCES",&left);panel(r,"LIVE PREVIEW",&middle,1);panel(r,"BROADCAST / AUDIO",&right);scenes=new QListWidget;left->addWidget(scenes,1);button(left,"+ Scene",[this]{auto n=QInputDialog::getText(this,"Scene","Name");if(!n.isEmpty()){addScene(n);save();}});sources=new QListWidget;left->addWidget(sources,2);auto *types=new QComboBox;types->addItems({"Display capture","Game capture","Window capture","Microphone","Desktop audio","Camera","Image","Media","Web widget","Test animation"});left->addWidget(types);button(left,"+ Add source",[this,types]{switch(types->currentIndex()){case 0:addSource("monitor_capture","Display");break;case 1:addSource("game_capture","Game");break;case 2:addSource("window_capture","Window");break;case 3:addSource("wasapi_input_capture","Microphone");break;case 4:addSource("wasapi_output_capture","Desktop audio");break;case 5:addSource("dshow_input","Camera");break;case 6:addMedia(true);break;case 7:addMedia(false);break;case 8:addWeb(false);break;case 9:addWeb(true);break;}});button(left,"Source settings",[this]{properties();});button(left,"Remove source",[this]{if(auto *i=item()){obs_sceneitem_remove(i);refresh();save();}});button(left,"Move source up",[this]{if(auto *i=item()){obs_sceneitem_set_order(i,OBS_ORDER_MOVE_UP);refresh();save();}});preview=new Preview;middle->addWidget(preview,1);auto *hint=new QLabel("1920 × 1080  /  30 FPS");middle->addWidget(hint);right->addWidget(new QLabel("YouTube RTMPS server"));server=new QLineEdit("rtmps://a.rtmps.youtube.com:443/live2");right->addWidget(server);right->addWidget(new QLabel("Stream key (not saved)"));key=new QLineEdit;key->setEchoMode(QLineEdit::Password);right->addWidget(key);live=button(right,"GO LIVE",[this]{startStream();});record=button(right,"RECORD",[this]{startRecord();});right->addWidget(new QLabel("Selected source volume"));volume=new QSlider(Qt::Horizontal);volume->setRange(0,100);right->addWidget(volume);mute=new QCheckBox("Mute selected source");right->addWidget(mute);right->addStretch();status=new QLabel("Ready");v->addWidget(status);connect(scenes,&QListWidget::currentRowChanged,this,[this]{if(scene())obs_set_output_source(0,obs_scene_get_source(scene()));refresh();});connect(sources,&QListWidget::currentRowChanged,this,[this]{audioUI();});connect(sources,&QListWidget::itemChanged,this,[this](QListWidgetItem *i){if(scene()){auto *s=obs_scene_find_sceneitem_by_id(scene(),i->data(Qt::UserRole).toLongLong());if(s){obs_sceneitem_set_visible(s,i->checkState()==Qt::Checked);save();}}});connect(volume,&QSlider::valueChanged,this,[this](int x){if(auto *i=item()){obs_source_set_volume(obs_sceneitem_get_source(i),float(x)/100);save();}});connect(mute,&QCheckBox::toggled,this,[this](bool x){if(auto *i=item()){obs_source_set_muted(obs_sceneitem_get_source(i),x);save();}});restore();if(sceneRefs.empty())addScene("Main");timer=new QTimer(this);connect(timer,&QTimer::timeout,this,[this]{tick();});timer->start(500);}
    ~Studio() override {timer->stop();save();preview->shutdown();if(stream)obs_output_force_stop(stream);if(rec)obs_output_force_stop(rec);releaseStream();releaseRecord();obs_set_output_source(0,nullptr);for(auto *s:sceneRefs)obs_scene_release(s);for(auto *s:restored)obs_source_release(s);}
};
int main(int argc,char **argv){QApplication app(argc,argv);app.setApplicationName("KASHARA Studio");app.setOrganizationName("KASHARA");QDir::setCurrent(QCoreApplication::applicationDirPath());QDir().mkpath(configDir()+"/plugins");if(!obs_startup("en-US",utf(configDir()+"/plugins").constData(),nullptr)){QMessageBox::critical(nullptr,"KASHARA","Cannot initialize LibOBS.");return 1;}
obs_audio_info ai{};ai.samples_per_sec=48000;ai.speakers=SPEAKERS_STEREO;
obs_video_info vi{};vi.graphics_module="libobs-d3d11.dll";vi.fps_num=30;vi.fps_den=1;vi.base_width=1920;vi.base_height=1080;vi.output_width=1920;vi.output_height=1080;vi.output_format=VIDEO_FORMAT_NV12;vi.gpu_conversion=true;vi.colorspace=VIDEO_CS_709;vi.range=VIDEO_RANGE_PARTIAL;vi.scale_type=OBS_SCALE_BICUBIC;
if(!obs_reset_audio(&ai)||obs_reset_video(&vi)!=0){QMessageBox::critical(nullptr,"KASHARA","Cannot initialize video/audio.");obs_shutdown();return 2;}
QString core=QDir("../../core").absolutePath();for(const QString &name:QStringList{"win-capture","win-wasapi","win-dshow","image-source","obs-ffmpeg","obs-x264","obs-outputs","rtmp-services","obs-browser","obs-filters"}){QString dir=core+"/"+name;QString dll=dir+"/"+name+".dll";if(!QFileInfo::exists(dll))continue;obs_module_t *module=nullptr;if(obs_open_module(&module,utf(dll).constData(),utf(dir+"/data").constData())==0)obs_init_module(module);}
obs_post_load_modules();
app.setStyleSheet("QWidget{background:#141414;color:#eeeeee;font-family:Segoe UI;font-size:13px;} QWidget#panel{background:#1b1b1b;border:1px solid #303030;} QLabel#brand{font-size:22px;font-weight:700;letter-spacing:2px;} QLabel#heading{font-size:12px;font-weight:700;color:#bbbbbb;padding:8px;} QPushButton{background:#292929;border:1px solid #444;padding:9px;border-radius:3px;} QPushButton:hover{background:#454545;} QLineEdit,QComboBox,QListWidget{background:#202020;border:1px solid #414141;padding:5px;} QListWidget::item{padding:7px;} QListWidget::item:selected{background:#555;color:white;} QSlider::groove:horizontal{height:4px;background:#444;} QSlider::handle:horizontal{width:12px;background:#ddd;margin:-4px 0;}");int result;{Studio studio;studio.show();result=app.exec();}obs_shutdown();return result;}
