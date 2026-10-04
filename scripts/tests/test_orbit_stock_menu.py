"""Preserve the four Chef destinations and Stock's ordinary question route."""

import unittest

from test_orbit_service_navigation import run_navigation


class StockMenuTests(unittest.TestCase):
    def test_four_destinations_and_capture_route(self):
        run_navigation(r'''
        Application app;Settings::writes=0;
        app.HandleOrbitMenuBlueOnMain();assert(app.IsOrbitMenuFace());
        for(unsigned page=1;page<=4;++page){
            app.HandleOrbitMenuBlueOnMain();assert(app.orbit_menu_index_==page%4);
        }
        const View destinations[]={View::Shopping,View::Home,View::Notes,View::Stock};
        for(unsigned page=0;page<4;++page){
            app.orbit_view_=View::Menu;app.orbit_menu_index_=page;app.dictation_screen_=true;
            assert(app.ConfirmOrbitMenu()==(page==1));assert(app.IsOrbitMenuFace());app.Drain();
            assert(app.orbit_view_==destinations[page] && !app.IsOrbitService());
            if(page==3){
                assert(app.IsOrbitStockFace() && !app.IsOrbitShoppingFace() && !app.IsOrbitNotesFace());
                assert(!app.dictation_screen_);
            }
        }
        // Blue returns to Stock's fourth menu card; browsing has no mode side effect.
        app.HandleOrbitMenuBlueOnMain();assert(app.IsOrbitMenuFace() && app.orbit_menu_index_==3);
        app.ConfirmOrbitMenu();app.Drain();assert(app.IsOrbitStockFace());
        app.HandleOrbitFaceSwipe(true);app.Drain();assert(app.IsOrbitShoppingFace());
        app.HandleOrbitFaceSwipe(false);app.Drain();assert(app.IsOrbitStockFace());
        app.HandleOrbitFaceSwipe(false);app.Drain();assert(app.IsOrbitNotesFace());
        for(bool right:{true,false})for(unsigned page=0;page<4;++page){
            app.HandleOrbitFaceSwipe(right);app.Drain();assert(!app.IsOrbitService());
        }
        assert(Settings::writes==0 && app.protocol->closed==0);
        ''')
